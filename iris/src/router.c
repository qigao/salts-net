#include "cors.h"
#include "iris.h"
#include "llhttp.h"
#include "middleware.h"
#include "route_trie.h"
#include "security.h"
#include "turbo_async_server.h"
#include "turbo_logger.h"
#include <ctype.h>
#include <stb_sprintf.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Internal implementation structure (hidden from public API)
struct http_parser_impl {
  llhttp_t parser;            // llhttp parser instance
  llhttp_settings_t settings; // llhttp parser settings
};

/* Forward declaration of connection context structure from server_refactored.c */
typedef struct {
  async_server_connection_t *connection;
  char buffer[8192]; /* READ_BUF_SIZE */
  size_t buffer_used;
  int keep_alive;
  time_t created_time;
  int request_count;
  void *middleware_data;
  void (*middleware_cleanup)(void *data);
} iris_connection_ctx_t;

// Write request structure definition (forward declared in router.h)
struct write_req_s {
  async_server_t *server;
  async_server_connection_t *connection;
  char *data; // Heap allocated (managed by caller)
};

// Sends error responses (400, 413, 414, or 500) - uses NetCore send
static void send_error(async_server_connection_t *connection, int error_code) {
  if (!connection)
    return;

  const char *err = NULL;
  const char *err_500 = "HTTP/1.1 500 Internal Server Error\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 21\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "Internal Server Error";

  const char *err_400 = "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 11\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "Bad Request";

  const char *err_413 = "HTTP/1.1 413 Payload Too Large\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 18\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "Payload Too Large";

  const char *err_414 = "HTTP/1.1 414 URI Too Long\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 12\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "URI Too Long";

  if (error_code == 500)
    err = err_500;
  else if (error_code == 400)
    err = err_400;
  else if (error_code == 413)
    err = err_413;
  else if (error_code == 414)
    err = err_414;
  else
    return;

  size_t len = strlen(err);
  async_server_status_t status = async_server_send(NULL, connection, err, len);
  if (status != ASYNC_SERVER_STATUS_OK) {
    LOG_ERROR("Send error: {}", async_server_status_to_string(status));
  }
}

// Separates URL into path and query string components
// Example: /users/123?active=true -> path="/users/123", query="active=true"
/* Phase IRIS-1: Updated to use turbo_arena_t */
static int extract_path_and_query(turbo_arena_t *arena, char *url_buf, char **path, char **query) {
  if (!arena || !url_buf || !path || !query)
    return -1;

  char *qmark = strchr(url_buf, '?');
  if (qmark) {
    *qmark = '\0';
    *path = turbo_arena_strdup(arena, url_buf);
    *query = turbo_arena_strdup(arena, qmark + 1);
  } else {
    *path = turbo_arena_strdup(arena, url_buf);
    *query = turbo_arena_strdup(arena, "");
  }

  if (!*path || !*query)
    return -1;

  // If path is empty, treat it as root
  if ((*path)[0] == '\0') {
    *path = turbo_arena_strdup(arena, "/");
    if (!*path)
      return -1;
  }
  return 0;
}

// Extracts URL parameters from a previously matched route
// Example: From route /users/:id matched with /users/123, extracts parameter id=123
/* Phase IRIS-1: Updated to use turbo_arena_t */
static int extract_url_params(turbo_arena_t *arena, const route_match_t *match,
                              request_t *url_params) {
  if (!arena || !match || !url_params)
    return -1;

  if (url_params->capacity == 0) {
    url_params->capacity = match->param_count > 0 ? match->param_count : 1;
    url_params->items = turbo_arena_alloc(arena, sizeof(request_item_t) * url_params->capacity);
    if (!url_params->items) {
      url_params->capacity = 0;
      return -1;
    }

    for (int i = 0; i < url_params->capacity; i++) {
      url_params->items[i].key = NULL;
      url_params->items[i].value = NULL;
    }
  }

  for (int i = 0; i < match->param_count && url_params->count < url_params->capacity; i++) {
    char *key = turbo_arena_alloc(arena, match->params[i].key.len + 1);
    char *value = turbo_arena_alloc(arena, match->params[i].value.len + 1);

    if (!key || !value) {
      return -1;
    }

    memcpy(key, match->params[i].key.data, match->params[i].key.len);
    key[match->params[i].key.len] = '\0';

    memcpy(value, match->params[i].value.data, match->params[i].value.len);
    value[match->params[i].value.len] = '\0';

    url_params->items[url_params->count].key = key;
    url_params->items[url_params->count].value = value;
    url_params->count++;
  }

  return 0;
}

// Context clearing
// Run at the begining of set_context and destroy_req
static void req_clear_context(Req *req) {
  if (!req)
    return;

  if (req->context.data && req->context.cleanup) {
    req->context.cleanup(req->context.data);
  }

  req->context.data = NULL;
  req->context.size = 0;
  req->context.cleanup = NULL;
  req->context.arena = NULL;
}

// Context management functions
void set_context(Req *req, void *data, size_t size, void (*cleanup)(void *)) {
  if (!req)
    return;

  // Clear existing context first
  req_clear_context(req);

  req->context.data = data;
  req->context.size = size;
  req->context.cleanup = cleanup;
  req->context.arena = req->arena;
}

void *get_context(Req *req) {
  if (!req)
    return NULL;
  return req->context.data;
}

/* ============================================================================
 * Connection Context Management Functions
 * ============================================================================ */

/**
 * @brief Set middleware-specific data on a connection
 * @param connection The connection to attach data to
 * @param data The data to attach
 * @param cleanup Cleanup function to call when connection is closed
 */
void set_connection_context(async_server_connection_t *connection, void *data,
                            void (*cleanup)(void *)) {
  if (!connection) {
    return;
  }

  /* Get the connection context from NetCore */
  void *ctx_ptr = async_server_connection_get_user_data(connection);
  if (!ctx_ptr) {
    /* No connection context exists - this shouldn't happen in normal operation */
    LOG_ERROR("Warning: Attempting to set connection context on connection without context");
    return;
  }

  /* Cast to our connection context structure */
  iris_connection_ctx_t *ctx = (iris_connection_ctx_t *)ctx_ptr;

  /* Clean up existing middleware data if present */
  if (ctx->middleware_data && ctx->middleware_cleanup) {
    ctx->middleware_cleanup(ctx->middleware_data);
  }

  /* Set new middleware data */
  ctx->middleware_data = data;
  ctx->middleware_cleanup = cleanup;
}

/**
 * @brief Get middleware-specific data from a connection
 * @param connection The connection to get data from
 * @return The middleware data, or NULL if none set
 */
void *get_connection_context(async_server_connection_t *connection) {
  if (!connection) {
    return NULL;
  }

  /* Get the connection context from NetCore */
  void *ctx_ptr = async_server_connection_get_user_data(connection);
  if (!ctx_ptr) {
    return NULL;
  }

  /* Cast to our connection context structure and return middleware data */
  iris_connection_ctx_t *ctx = (iris_connection_ctx_t *)ctx_ptr;
  return ctx->middleware_data;
}

// Create and initialize Req
/* Phase IRIS-1: Updated to use turbo_arena_t */
static Req *create_req(turbo_arena_t *arena, async_server_connection_t *connection) {
  if (!arena)
    return NULL;

  /* Phase IRIS-1: Allocate Req from arena */
  Req *req = turbo_arena_alloc(arena, sizeof(Req));
  if (!req)
    return NULL;

  memset(req, 0, sizeof(Req));
  req->arena = arena;           /* Phase IRIS-1: Store pointer to shared arena */
  req->connection = connection; /* NetCore migration: use connection instead of client_socket */
  req->method = NULL;
  req->path = NULL;
  req->body = NULL;
  req->body_len = 0;

  // Initialize request_t structures
  memset(&req->headers, 0, sizeof(request_t));
  memset(&req->query, 0, sizeof(request_t));
  memset(&req->params, 0, sizeof(request_t));

  // Initialize context
  req->context.data = NULL;
  req->context.size = 0;
  req->context.cleanup = NULL;
  req->context.arena = arena;

  // Initialize security context
  req->security = turbo_arena_alloc(arena, sizeof(iris_security_context_t));
  if (!req->security) {
    return NULL;
  }
  iris_security_context_init(req->security);
  req->request_start_time = time(NULL);

  return req;
}

// Create and initialize Res
/* Phase IRIS-1: Updated to use turbo_arena_t */
static Res *create_res(turbo_arena_t *arena, async_server_connection_t *connection) {
  if (!arena)
    return NULL;

  /* Phase IRIS-1: Allocate Res from arena */
  Res *res = turbo_arena_alloc(arena, sizeof(Res));
  if (!res)
    return NULL;

  memset(res, 0, sizeof(Res));
  res->arena = arena;           /* Phase IRIS-1: Store pointer to shared arena */
  res->connection = connection; /* NetCore migration: use connection instead of client_socket */
  res->status = 200;
  res->content_type = turbo_arena_strdup(arena, "text/plain"); /* Phase IRIS-1: Updated */
  res->body = NULL;
  res->body_len = 0;
  res->keep_alive = 1;
  res->headers = NULL;
  res->header_count = 0;
  res->header_capacity = 0;

  return res;
}

// Create and initialize http_context_t
/* Phase IRIS-1: Updated to use turbo_arena_t */
static http_context_t *create_http_context(turbo_arena_t *arena) {
  if (!arena)
    return NULL;

  http_context_t *context =
      turbo_arena_alloc(arena, sizeof(http_context_t)); /* Phase IRIS-1: Updated */
  if (!context)
    return NULL;

  http_context_init(context, arena);
  return context;
}

// Cleanup function for request_t
static void cleanup_request_t(request_t *req_data) {
  if (!req_data)
    return;

  for (int i = 0; i < req_data->count; i++) {
    free(req_data->items[i].key);
    free(req_data->items[i].value);
  }

  free(req_data->items);
  req_data->items = NULL;
  req_data->count = 0;
  req_data->capacity = 0;
}

/* Phase IRIS-1: Updated to use turbo_arena_t */
request_t copy_request_t(turbo_arena_t *arena, const request_t *original) {
  request_t copy;
  memset(&copy, 0, sizeof(request_t));

  if (!original || original->count == 0)
    return copy;

  if (arena) {
    // Allocate items array in arena
    copy.capacity = original->capacity;
    copy.count = original->count;
    copy.items = turbo_arena_alloc(arena, copy.capacity * sizeof(request_item_t));

    if (!copy.items) {
      copy.capacity = 0;
      copy.count = 0;
      return copy;
    }

    // Copy each item using arena
    for (int i = 0; i < original->count; i++) {
      if (original->items[i].key) {
        copy.items[i].key = turbo_arena_strdup(arena, original->items[i].key);
        if (!copy.items[i].key) {
          // Arena allocation failed - clear and return
          memset(&copy, 0, sizeof(request_t));
          return copy;
        }
      } else {
        copy.items[i].key = NULL;
      }

      if (original->items[i].value) {
        copy.items[i].value = turbo_arena_strdup(arena, original->items[i].value);
        if (!copy.items[i].value) {
          // Arena allocation failed - clear and return
          memset(&copy, 0, sizeof(request_t));
          return copy;
        }
      } else {
        copy.items[i].value = NULL;
      }
    }

    return copy;
  } else {
    // Allocate items array
    copy.capacity = original->capacity;
    copy.count = original->count;
    copy.items = malloc(copy.capacity * sizeof(request_item_t));

    if (!copy.items) {
      // Allocation failed, return empty
      copy.capacity = 0;
      copy.count = 0;
      return copy;
    }

    // Copy each item
    for (int i = 0; i < original->count; i++) {
      // Copy key
      if (original->items[i].key) {
        copy.items[i].key = strdup(original->items[i].key);
        if (!copy.items[i].key) {
          // Cleanup on failure
          for (int j = 0; j < i; j++) {
            free(copy.items[j].key);
            free(copy.items[j].value);
          }
          free(copy.items);
          memset(&copy, 0, sizeof(request_t));
          return copy;
        }
      } else {
        copy.items[i].key = NULL;
      }

      // Copy value
      if (original->items[i].value) {
        copy.items[i].value = strdup(original->items[i].value);
        if (!copy.items[i].value) {
          // Cleanup on failure
          free(copy.items[i].key);
          for (int j = 0; j < i; j++) {
            free(copy.items[j].key);
            free(copy.items[j].value);
          }
          free(copy.items);
          memset(&copy, 0, sizeof(request_t));
          return copy;
        }
      } else {
        copy.items[i].value = NULL;
      }
    }

    return copy;
  }
}

// Destroy Req
void destroy_req(Req *req) {
  if (!req)
    return;

  req_clear_context(req);

  // Free allocated strings
  if (req->method) {
    free((void *)req->method);
    req->method = NULL;
  }

  if (req->path) {
    free((void *)req->path);
    req->path = NULL;
  }

  if (req->body) {
    free(req->body);
    req->body = NULL;
  }

  // Free request_t structures
  cleanup_request_t(&req->headers);
  cleanup_request_t(&req->query);
  cleanup_request_t(&req->params);

  // Free security context (only if not arena-allocated)
  if (req->security && !req->arena) {
    free(req->security);
    req->security = NULL;
  }

  free(req);
}

// Destroy Res
void destroy_res(Res *res) {
  if (!res)
    return;

  if (res->headers) {
    for (int i = 0; i < res->header_count; i++) {
      free(res->headers[i].name);
      free(res->headers[i].value);
    }
    free(res->headers);
    res->headers = NULL;
  }
  res->header_count = 0;
  res->header_capacity = 0;

  free(res);
}

// Arena-aware request population
static int populate_req_from_context(Req *req, http_context_t *context, const char *path) {
  if (!req || !req->arena || !context)
    return -1;

  turbo_arena_t *arena = req->arena;

  // Copy method
  if (context->method) {
    req->method = turbo_arena_strdup(arena, context->method);
    if (!req->method)
      return -1;
  }

  // Copy path
  if (path) {
    req->path = turbo_arena_strdup(arena, path);
    if (!req->path)
      return -1;
  }

  // Copy body
  if (context->body && context->body_length > 0) {
    req->body = turbo_arena_alloc(arena, context->body_length + 1);
    if (!req->body)
      return -1;
    memcpy(req->body, context->body, context->body_length);
    req->body[context->body_length] = '\0';
    req->body_len = context->body_length;
  }

  req->headers = copy_request_t(arena, &context->headers);
  req->query = copy_request_t(arena, &context->query_params);
  req->params = copy_request_t(arena, &context->url_params);

  return 0;
}

// Composes and sends the response (headers + body) using NetCore send
void reply(Res *res, int status, const char *content_type, const void *body, size_t body_len) {
  if (!res || !res->connection) {
    return;
  }

  if (!content_type)
    content_type = "text/plain";
  if (!body)
    body_len = 0;

  // Apply output escaping based on content type if body contains user data
  const void *escaped_body = body;
  size_t escaped_body_len = body_len;
  char *escaped_buffer = NULL;

  if (body && body_len > 0) {
    // Determine if escaping is needed based on content type
    bool needs_escaping = false;
    iris_security_result_t (*escape_func)(const char *, char *, size_t) = NULL;

    if (strstr(content_type, "text/html") != NULL) {
      needs_escaping = true;
      escape_func = iris_escape_html;
    } else if (strstr(content_type, "application/json") != NULL) {
      needs_escaping = true;
      escape_func = iris_escape_json;
    } else if (strstr(content_type, "application/javascript") != NULL ||
               strstr(content_type, "text/javascript") != NULL) {
      needs_escaping = true;
      escape_func = iris_escape_javascript;
    }

    if (needs_escaping && escape_func) {
      // Allocate buffer for escaped content (estimate 2x original size)
      size_t escaped_buffer_size = body_len * 2 + 256;
      escaped_buffer = malloc(escaped_buffer_size);

      if (escaped_buffer) {
        iris_security_result_t escape_result =
            escape_func((const char *)body, escaped_buffer, escaped_buffer_size);

        if (escape_result == IRIS_SECURITY_OK) {
          escaped_body = escaped_buffer;
          escaped_body_len = strlen(escaped_buffer);
          // Mark that output has been escaped for security tracking
          if (res->arena) {
            // We don't have direct access to req here, but we can add a flag to res if needed
            // For now, just log that escaping was applied
            LOG_ERROR("Security: Output escaped for content type: {}", content_type);
          }
        } else if (escape_result == IRIS_SECURITY_ERROR_BUFFER_TOO_SMALL) {
          // Try with larger buffer
          free(escaped_buffer);
          escaped_buffer_size = body_len * 4 + 512;
          escaped_buffer = malloc(escaped_buffer_size);

          if (escaped_buffer) {
            escape_result = escape_func((const char *)body, escaped_buffer, escaped_buffer_size);
            if (escape_result == IRIS_SECURITY_OK) {
              escaped_body = escaped_buffer;
              escaped_body_len = strlen(escaped_buffer);
              // Mark that output has been escaped for security tracking
              LOG_ERROR("Security: Output escaped for content type: {} (retry)", content_type);
            } else {
              // Escaping failed, log warning and use original content
              LOG_ERROR("Warning: Output escaping failed: {}",
                        iris_security_error_string(escape_result));
              free(escaped_buffer);
              escaped_buffer = NULL;
            }
          }
        } else {
          // Escaping failed, log warning and use original content
          LOG_ERROR("Warning: Output escaping failed: {}",
                    iris_security_error_string(escape_result));
          free(escaped_buffer);
          escaped_buffer = NULL;
        }
      }
    }
  }

  // Get current date in HTTP format
  time_t now = time(NULL);
  struct tm *gmt = gmtime(&now);
  char date_str[64];
  strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", gmt);

  // Calculate total size of custom headers
  size_t headers_size = 0;
  for (int i = 0; i < res->header_count; i++) {
    if (res->headers[i].name && res->headers[i].value) {
      headers_size += strlen(res->headers[i].name) + 2 + strlen(res->headers[i].value) + 2;
    }
  }

  // Allocate and fill entire header string using malloc
  char *all_headers = malloc(headers_size + 1);
  if (!all_headers) {
    send_error(res->connection, 500);
    return;
  }

  size_t pos = 0;
  for (int i = 0; i < res->header_count; i++) {
    if (res->headers[i].name && res->headers[i].value) {
      int n = stbsp_snprintf(all_headers + pos, (int)(headers_size - pos + 1), "%s: %s\r\n",
                             res->headers[i].name, res->headers[i].value);
      if (n > 0 && (size_t)n <= headers_size - pos) {
        pos += n;
      } else {
        free(all_headers);
        send_error(res->connection, 500);
        return;
      }
    }
  }
  all_headers[pos] = '\0';

  // Calculate response size
  int base_header_len = stbsp_snprintf(NULL, 0,
                                       "HTTP/1.1 %d\r\n"
                                       "Server: Ecewo\r\n"
                                       "Date: %s\r\n"
                                       "%s"
                                       "Content-Type: %s\r\n"
                                       "Content-Length: %zu\r\n"
                                       "Connection: %s\r\n"
                                       "\r\n",
                                       status, date_str, all_headers, content_type,
                                       escaped_body_len, res->keep_alive ? "keep-alive" : "close");

  if (base_header_len < 0) {
    free(all_headers);
    if (escaped_buffer)
      free(escaped_buffer);
    send_error(res->connection, 500);
    return;
  }

  size_t total_len = (size_t)base_header_len + escaped_body_len;

  // Use malloc for response buffer
  char *response = malloc(total_len + 1);
  if (!response) {
    free(all_headers);
    if (escaped_buffer)
      free(escaped_buffer);
    send_error(res->connection, 500);
    return;
  }

  int written = stbsp_snprintf(response, (size_t)base_header_len + 1,
                               "HTTP/1.1 %d\r\n"
                               "Server: Ecewo\r\n"
                               "Date: %s\r\n"
                               "%s"
                               "Content-Type: %s\r\n"
                               "Content-Length: %zu\r\n"
                               "Connection: %s\r\n"
                               "\r\n",
                               status, date_str, all_headers, content_type, escaped_body_len,
                               res->keep_alive ? "keep-alive" : "close");

  free(all_headers);

  if (written < 0 || (size_t)written > total_len) {
    free(response);
    if (escaped_buffer)
      free(escaped_buffer);
    send_error(res->connection, 500);
    return;
  }

  if (escaped_body_len > 0 && escaped_body) {
    memcpy(response + written, escaped_body, escaped_body_len);
  }

  // Send using NetCore API
  async_server_status_t result = async_server_send(NULL, res->connection, response, total_len);
  if (result != ASYNC_SERVER_STATUS_OK) {
    LOG_ERROR("Send error: {}", async_server_status_to_string(result));
  }

  // Free the response buffer immediately since NetCore copies the data
  free(response);

  // Free escaped buffer if allocated
  if (escaped_buffer) {
    free(escaped_buffer);
  }
}

// Validates all cookies in the Cookie header
static iris_security_result_t validate_request_cookies(http_context_t *ctx) {
  if (!ctx) {
    return IRIS_SECURITY_ERROR_NULL_POINTER;
  }

  // Find the Cookie header
  const char *cookie_header = NULL;
  for (int i = 0; i < ctx->headers.count; i++) {
    if (ctx->headers.items[i].key && strcasecmp(ctx->headers.items[i].key, "Cookie") == 0) {
      cookie_header = ctx->headers.items[i].value;
      break;
    }
  }

  // If no cookies, that's fine
  if (!cookie_header) {
    return IRIS_SECURITY_OK;
  }

  const iris_security_limits_t *limits = iris_security_get_limits();
  const char *pos = cookie_header;

  // Parse and validate each cookie in the header
  while (pos && *pos) {
    // Skip whitespace
    while (*pos && isspace((unsigned char)*pos))
      pos++;

    if (!*pos)
      break;

    // Find the cookie name
    const char *name_start = pos;
    const char *equals = strchr(pos, '=');
    if (!equals) {
      // Malformed cookie - no equals sign
      return IRIS_SECURITY_ERROR_INVALID_FORMAT;
    }

    // Extract and validate cookie name
    size_t name_len = (size_t)(equals - name_start);
    char *name = malloc(name_len + 1);
    if (!name) {
      return IRIS_SECURITY_ERROR_INVALID_INPUT;
    }
    memcpy(name, name_start, name_len);
    name[name_len] = '\0';

    // Trim trailing whitespace from name
    while (name_len > 0 && isspace((unsigned char)name[name_len - 1])) {
      name[--name_len] = '\0';
    }

    iris_security_result_t name_validation =
        iris_validate_cookie_name(name, limits->max_cookie_name_length);
    if (name_validation != IRIS_SECURITY_OK) {
      LOG_ERROR("Security: Invalid cookie name in request: {} (error: {})", name,
                iris_security_error_string(name_validation));
      free(name);
      return name_validation;
    }

    // Move to cookie value
    pos = equals + 1;

    // Find the end of the value (either ';' or end of string)
    const char *value_start = pos;
    const char *semicolon = strchr(pos, ';');
    size_t value_len = semicolon ? (size_t)(semicolon - value_start) : strlen(value_start);

    // Extract and validate cookie value
    char *value = malloc(value_len + 1);
    if (!value) {
      free(name);
      return IRIS_SECURITY_ERROR_INVALID_INPUT;
    }
    memcpy(value, value_start, value_len);
    value[value_len] = '\0';

    // Trim trailing whitespace from value
    while (value_len > 0 && isspace((unsigned char)value[value_len - 1])) {
      value[--value_len] = '\0';
    }

    iris_security_result_t value_validation =
        iris_validate_cookie_value(value, limits->max_cookie_value_length);
    if (value_validation != IRIS_SECURITY_OK) {
      LOG_ERROR("Security: Invalid cookie value for '{}' (error: {})", name,
                iris_security_error_string(value_validation));
      free(name);
      free(value);
      return value_validation;
    }

    free(name);
    free(value);

    // Move to next cookie
    if (semicolon) {
      pos = semicolon + 1;
    } else {
      break;
    }
  }

  return IRIS_SECURITY_OK;
}

// Main router function
int router(async_server_connection_t *connection, const char *request_data, size_t request_len) {
  if (!connection || !request_data || request_len == 0) {
    if (connection)
      send_error(connection, 400);
    return 1;
  }

  // Phase IRIS-1: Create request arena (8KB initial - enough for typical HTTP request)
  turbo_arena_t arena;
  if (turbo_arena_init(&arena, 8192) != 0) {
    send_error(connection, 500);
    return 1; // Close connection on arena init failure
  }

  // Initialize all resources
  http_context_t *ctx = NULL;
  Req *req = NULL;
  Res *res = NULL;
  tokenized_path_t tokenized_path = {0};
  int error_code = 0;
  int should_close = 1;
  bool send_error_response = false;
  bool send_404_response = false;

  // Create resources
  ctx = create_http_context(&arena);
  req = create_req(&arena, connection);
  res = create_res(&arena, connection);

  if (!ctx || !req || !res) {
    error_code = 500;
    send_error_response = true;
    goto cleanup;
  }

  // Parse HTTP request
  enum llhttp_errno err = llhttp_execute(&ctx->parser_impl->parser, request_data, request_len);
  if (err != HPE_OK) {
    if (err == HPE_USER) {
      // HPE_USER indicates payload too large (from on_body_cb)
      error_code = 413;
    } else {
      error_code = 400;
    }
    send_error_response = true;
    goto cleanup;
  }

  // Extract path and query
  char *path = NULL;
  char *query = NULL;
  if (extract_path_and_query(&arena, ctx->url, &path, &query) != 0) {
    error_code = 500;
    send_error_response = true;
    goto cleanup;
  }

  if (!path) {
    error_code = 400;
    send_error_response = true;
    goto cleanup;
  }

  // Validate URL path for security
  const iris_security_limits_t *limits = iris_security_get_limits();
  iris_security_result_t path_validation = iris_validate_url_path(path, limits->max_url_length);
  if (path_validation != IRIS_SECURITY_OK) {
    if (path_validation == IRIS_SECURITY_ERROR_MALICIOUS_CONTENT) {
      // Log potential attack attempt
      LOG_ERROR("Security: Malicious URL path detected: {}", path);
      error_code = 400;
    } else if (path_validation == IRIS_SECURITY_ERROR_SIZE_EXCEEDED) {
      error_code = 414; // URI Too Long
    } else {
      error_code = 400;
    }
    send_error_response = true;
    goto cleanup;
  }
  req->security->url_validated = true;

  // Validate cookies for security
  iris_security_result_t cookie_validation = validate_request_cookies(ctx);
  if (cookie_validation != IRIS_SECURITY_OK) {
    if (cookie_validation == IRIS_SECURITY_ERROR_MALICIOUS_CONTENT) {
      // Log potential attack attempt
      LOG_ERROR("Security: Malicious cookie content detected\n");
      error_code = 400;
    } else if (cookie_validation == IRIS_SECURITY_ERROR_SIZE_EXCEEDED) {
      error_code = 400; // Bad Request - cookie too large
    } else {
      error_code = 400;
    }
    send_error_response = true;
    goto cleanup;
  }
  req->security->cookies_validated = true;

  // Parse query parameters
  parse_query(&arena, query, &ctx->query_params);
  res->keep_alive = ctx->keep_alive;

  // Handle CORS preflight
  if (cors_handle_preflight(ctx, res)) {
    reply(res, res->status, res->content_type, res->body, res->body_len);
    should_close = !res->keep_alive;
    goto cleanup;
  }

  // Route matching validation
  if (!global_route_trie || !ctx->method) {
    LOG_ERROR("Missing route trie ({}) or method ({})", global_route_trie,
              ctx->method ? ctx->method : "NULL");
    cors_add_headers(ctx, res);
    send_404_response = true;
    goto cleanup;
  }

  // Tokenize path
  if (tokenize_path(&arena, path, &tokenized_path) != 0) {
    error_code = 500;
    send_error_response = true;
    goto cleanup;
  }

  // Route matching
  route_match_t match;
  if (!route_trie_match(global_route_trie, ctx->method, &tokenized_path, &match)) {
    cors_add_headers(ctx, res);
    send_404_response = true;
    goto cleanup;
  }

  if (extract_url_params(&arena, &match, &ctx->url_params) != 0) {
    error_code = 500;
    send_error_response = true;
    goto cleanup;
  }

  if (populate_req_from_context(req, ctx, path) != 0) {
    error_code = 500;
    send_error_response = true;
    goto cleanup;
  }

  if (!match.handler) {
    error_code = 500;
    send_error_response = true;
    goto cleanup;
  }

  // Success path - call handler
  cors_add_headers(ctx, res);

  // Calls chain if there is a middleware
  // otherwise, calls the handler directly
  if (match.middleware_ctx) {
    MiddlewareInfo *middleware_info = (MiddlewareInfo *)match.middleware_ctx;
    execute_middleware_chain(req, res, middleware_info);
  } else {
    // No middleware, call handler directly
    match.handler(req, res);
  }

  should_close = !res->keep_alive;
  goto cleanup;

cleanup:
  // Send error responses if needed
  if (send_error_response && error_code > 0) {
    send_error(connection, error_code);
  } else if (send_404_response) {
    const char *not_found_msg = "404 Not Found";
    reply(res, 404, "text/plain", not_found_msg, strlen(not_found_msg));
    should_close = !res->keep_alive;
  }

  // Phase IRIS-1: Free the entire arena (handles all request/response memory)
  turbo_arena_free(&arena);

  return should_close;
}

// Adds a header
void set_header(Res *res, const char *name, const char *value) {
  if (!res || !name || !value) {
    LOG_ERROR("Error: Invalid argument(s) to set_header\n");
    return;
  }

  if (res->header_count >= res->header_capacity) {
    int new_cap = res->header_capacity ? res->header_capacity * 2 : 8;
    http_header_t *tmp;

    if (res->arena) {
      // Arena-based allocation
      /* Phase IRIS-1: turbo_arena doesn't have realloc, so alloc + memcpy */
      tmp = turbo_arena_alloc(res->arena, new_cap * sizeof(http_header_t));
      if (tmp && res->headers && res->header_capacity > 0) {
        memcpy(tmp, res->headers, res->header_capacity * sizeof(http_header_t));
      }
    } else {
      // Malloc-based allocation
      tmp = realloc(res->headers, new_cap * sizeof(http_header_t));
    }

    if (!tmp) {
      LOG_ERROR("Error: Failed to realloc headers array\n");
      return;
    }

    res->headers = tmp;
    res->header_capacity = new_cap;
  }

  if (res->arena) {
    // Arena-based string allocation
    res->headers[res->header_count].name = turbo_arena_strdup(res->arena, name);
    res->headers[res->header_count].value = turbo_arena_strdup(res->arena, value);
  } else {
    // Malloc-based string allocation
    res->headers[res->header_count].name = strdup(name);
    res->headers[res->header_count].value = strdup(value);
  }

  if (!res->headers[res->header_count].name || !res->headers[res->header_count].value) {
    LOG_ERROR("Error: Failed to allocate memory for header strings\n");
    // Cleanup on failure
    if (!res->arena) {
      free(res->headers[res->header_count].name);
      free(res->headers[res->header_count].value);
    }
    return;
  }

  res->header_count++;
}

// Deep copy function for Res
Res *copy_res(const Res *original) {
  if (!original)
    return NULL;

  Res *copy = malloc(sizeof(Res));
  if (!copy)
    return NULL;

  // Copy primitive fields
  *copy = *original;
  copy->arena = NULL;
  copy->body = original->body; // pointer only, not deep copied
  copy->content_type = original->content_type;

  // Allocate and copy headers array
  if (original->header_capacity > 0) {
    size_t cap = original->header_capacity;
    copy->headers = malloc(cap * sizeof(http_header_t));
    if (!copy->headers) {
      free(copy);
      return NULL;
    }

    for (int i = 0; i < original->header_count; ++i) {
      // Duplicate name
      if (original->headers[i].name) {
        copy->headers[i].name = strdup(original->headers[i].name);
        if (!copy->headers[i].name) {
          destroy_res(copy);
          return NULL;
        }
      } else {
        copy->headers[i].name = NULL;
      }
      // Duplicate value
      if (original->headers[i].value) {
        copy->headers[i].value = strdup(original->headers[i].value);
        if (!copy->headers[i].value) {
          destroy_res(copy);
          return NULL;
        }
      } else {
        copy->headers[i].value = NULL;
      }
    }
  } else {
    copy->headers = NULL;
  }

  return copy;
}

// Deep copy function for Req
Req *copy_req(const Req *original) {
  if (!original)
    return NULL;

  Req *copy = malloc(sizeof(Req));
  if (!copy)
    return NULL;

  // Copy primitive fields
  copy->arena = NULL;
  copy->connection = original->connection; /* NetCore migration: use connection */
  copy->body_len = original->body_len;

  // Deep copy method string
  if (original->method) {
    copy->method = strdup(original->method);
    if (!copy->method) {
      free(copy);
      return NULL;
    }
  } else {
    copy->method = NULL;
  }

  // Deep copy path string
  if (original->path) {
    copy->path = strdup(original->path);
    if (!copy->path) {
      if (copy->method)
        free((void *)copy->method);
      free(copy);
      return NULL;
    }
  } else {
    copy->path = NULL;
  }

  // Deep copy body
  if (original->body && original->body_len > 0) {
    copy->body = malloc(original->body_len + 1);
    if (!copy->body) {
      if (copy->method)
        free((void *)copy->method);
      if (copy->path)
        free((void *)copy->path);
      free(copy);
      return NULL;
    }
    memcpy(copy->body, original->body, original->body_len);
    copy->body[original->body_len] = '\0'; // Null terminate
  } else {
    copy->body = NULL;
  }

  // Deep copy headers, query, params
  copy->headers = copy_request_t(NULL, &original->headers);
  copy->query = copy_request_t(NULL, &original->query);
  copy->params = copy_request_t(NULL, &original->params);

  // Initialize context (don't copy original context, start fresh)
  memset(&copy->context, 0, sizeof(copy->context));
  copy->context.data = NULL;
  copy->context.size = 0;
  copy->context.cleanup = NULL;

  // Copy security context
  copy->security = malloc(sizeof(iris_security_context_t));
  if (!copy->security) {
    if (copy->method)
      free((void *)copy->method);
    if (copy->path)
      free((void *)copy->path);
    if (copy->body)
      free(copy->body);
    free(copy);
    return NULL;
  }
  if (original->security) {
    *copy->security = *original->security;
  } else {
    iris_security_context_init(copy->security);
  }
  copy->request_start_time = original->request_start_time;

  return copy;
}

/* Phase IRIS-1: Updated to use turbo_arena_t */
Req *arena_copy_req(turbo_arena_t *target_arena, const Req *original) {
  if (!original || !target_arena)
    return NULL;

  // Allocate on target arena
  Req *copy = turbo_arena_alloc(target_arena, sizeof(Req));
  if (!copy)
    return NULL;

  // Copy primitive fields
  copy->arena = target_arena;
  copy->connection = original->connection; /* NetCore migration: use connection */
  copy->body_len = original->body_len;

  // Deep copy strings using target arena
  if (original->method)
    copy->method = turbo_arena_strdup(target_arena, original->method);

  if (original->path)
    copy->path = turbo_arena_strdup(target_arena, original->path);

  if (original->body && original->body_len > 0) {
    copy->body = turbo_arena_alloc(target_arena, original->body_len + 1);
    memcpy(copy->body, original->body, original->body_len);
    copy->body[original->body_len] = '\0';
  }

  // Deep copy request_t structures using target arena
  copy->headers = copy_request_t(target_arena, &original->headers);
  copy->query = copy_request_t(target_arena, &original->query);
  copy->params = copy_request_t(target_arena, &original->params);

  // Initialize context
  memset(&copy->context, 0, sizeof(copy->context));
  copy->context.arena = target_arena;

  // Copy security context using target arena
  copy->security = turbo_arena_alloc(target_arena, sizeof(iris_security_context_t));
  if (!copy->security) {
    return NULL;
  }
  if (original->security) {
    *copy->security = *original->security;
  } else {
    iris_security_context_init(copy->security);
  }
  copy->request_start_time = original->request_start_time;

  return copy;
}

/* Phase IRIS-1: Updated to use turbo_arena_t */
Res *arena_copy_res(turbo_arena_t *target_arena, const Res *original) {
  if (!original || !target_arena)
    return NULL;

  // Allocate on target arena
  Res *copy = turbo_arena_alloc(target_arena, sizeof(Res));
  if (!copy)
    return NULL;

  // Copy primitive fields
  *copy = *original;
  copy->arena = target_arena;

  if (original->content_type)
    copy->content_type = turbo_arena_strdup(target_arena, original->content_type);

  // Copy headers array in target arena
  if (original->header_capacity > 0) {
    copy->headers =
        turbo_arena_alloc(target_arena, original->header_capacity * sizeof(http_header_t));

    for (int i = 0; i < original->header_count; ++i) {
      if (original->headers[i].name)
        copy->headers[i].name = turbo_arena_strdup(target_arena, original->headers[i].name);
      if (original->headers[i].value)
        copy->headers[i].value = turbo_arena_strdup(target_arena, original->headers[i].value);
    }
  }

  return copy;
}
