#include "cors.h"
#include "iris.h"
#include "llhttp.h"
#include "middleware.h"
#include "route_trie.h"
#include "security.h"
#include "CoroNet/turbo_coro_socket.h"
#include "turbo_str.h"
#include "tlog.h"
#include <fmt.h>
#include <ctype.h>
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
  struct iris_app *app;
  coro_socket_t *client;
  char *buffer;
  size_t buffer_capacity;
  size_t buffer_used;
  size_t parsed_offset;
  int keep_alive;
  time_t created_time;
  int request_count;
  mem_pool_t request_arena;
  http_context_t *request_ctx;
  int request_arena_ready;
  int request_streaming_active;
  const char *pending_body_chunk;
  size_t pending_body_chunk_len;
  size_t pending_body_chunk_offset;
  void *middleware_data;
  void (*middleware_cleanup)(void *data);
} iris_connection_ctx_t;

// Write request structure definition (forward declared in router.h)
// Write request structure definition (forward declared in router.h)
struct write_req_s {
  coro_socket_t *client;
  char *data; // Heap allocated (managed by caller)
};

// Sends error responses (400, 413, 414, or 500) - uses CoroNet send
static void send_error(coro_socket_t *client, int error_code) {
  if (!client)
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
  int status = coro_socket_send(client, err, len);
  if (status != 0) {
    TLOG_ERROR("Send error: %d", status);
  }
}

static int send_res_data(Res *res, const void *data, size_t len, const char *what) {
  if (!res || !res->client || !data || len == 0) {
    return -1;
  }

  int rc = coro_socket_send(res->client, data, len);
  if (rc != 0) {
    TLOG_ERROR("Failed to send {}: {}", what ? what : "response data", rc);
    res->keep_alive = 0;
    return -1;
  }

  return 0;
}

// Separates URL into path and query string components
// Example: /users/123?active=true -> path="/users/123", query="active=true"
/* Phase IRIS-1: Updated to use mem_pool_t */
static int extract_path_and_query(mem_pool_t *arena, char *url_buf, char **path, char **query) {
  if (!arena || !url_buf || !path || !query)
    return -1;

  char *qmark = strchr(url_buf, '?');
  if (qmark) {
    *qmark = '\0';
    *path = mem_strdup(arena, url_buf);
    *query = mem_strdup(arena, qmark + 1);
  } else {
    *path = mem_strdup(arena, url_buf);
    *query = mem_strdup(arena, "");
  }

  if (!*path || !*query)
    return -1;

  // If path is empty, treat it as root
  if ((*path)[0] == '\0') {
    *path = mem_strdup(arena, "/");
    if (!*path)
      return -1;
  }
  return 0;
}

// Extracts URL parameters from a previously matched route
// Example: From route /users/:id matched with /users/123, extracts parameter id=123
/* Phase IRIS-1: Updated to use mem_pool_t */
static int extract_url_params(mem_pool_t *arena, const route_match_t *match,
                              request_t *url_params) {
  if (!arena || !match || !url_params)
    return -1;

  if (url_params->capacity == 0) {
    url_params->capacity = match->param_count > 0 ? match->param_count : 1;
    url_params->items = mem_alloc(arena, sizeof(request_item_t) * url_params->capacity);
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
    char *key = mem_alloc(arena, match->params[i].key.len + 1);
    char *value = mem_alloc(arena, match->params[i].value.len + 1);

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
 * @param client The connection to attach data to
 * @param data The data to attach
 * @param cleanup Cleanup function to call when connection is closed
 */
void set_connection_context(coro_socket_t *client, void *data,
                            void (*cleanup)(void *)) {
  if (!client) {
    return;
  }

  /* Get the connection context from CoroNet */
  void *ctx_ptr = coro_socket_get_user_data(client);
  if (!ctx_ptr) {
    /* No connection context exists - this shouldn't happen in normal operation */
    TLOG_ERROR("Warning: Attempting to set connection context on connection without context");
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
 * @param client The connection to get data from
 * @return The middleware data, or NULL if none set
 */
void *get_connection_context(coro_socket_t *client) {
  if (!client) {
    return NULL;
  }

  /* Get the connection context from CoroNet */
  void *ctx_ptr = coro_socket_get_user_data(client);
  if (!ctx_ptr) {
    return NULL;
  }

  /* Cast to our connection context structure and return middleware data */
  iris_connection_ctx_t *ctx = (iris_connection_ctx_t *)ctx_ptr;
  return ctx->middleware_data;
}

// Create and initialize Req
/* Phase IRIS-1: Updated to use mem_pool_t */
static Req *create_req(mem_pool_t *arena, struct iris_app *app, coro_socket_t *client) {
  if (!arena)
    return NULL;

  /* Phase IRIS-1: Allocate Req from arena */
  Req *req = mem_alloc(arena, sizeof(Req));
  if (!req)
    return NULL;

  memset(req, 0, sizeof(Req));
  req->app = app;               /* Association with application instance */
  req->arena = arena;           /* Phase IRIS-1: Store pointer to shared arena */
  req->client = client;         /* CoroNet migration: use client */
  req->method = NULL;
  req->path = NULL;
  req->body = NULL;
  req->body_len = 0;
  req->body_stream = 0;
  req->body_read_total = 0;

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
  req->security = mem_alloc(arena, sizeof(iris_security_context_t));
  if (!req->security) {
    return NULL;
  }
  iris_security_context_init(req->security);
  req->request_start_time = time(NULL);

  return req;
}

// Create and initialize Res
/* Phase IRIS-1: Updated to use mem_pool_t */
static Res *create_res(mem_pool_t *arena, coro_socket_t *client) {
  if (!arena)
    return NULL;

  /* Phase IRIS-1: Allocate Res from arena */
  Res *res = mem_alloc(arena, sizeof(Res));
  if (!res)
    return NULL;

  memset(res, 0, sizeof(Res));
  res->arena = arena;           /* Phase IRIS-1: Store pointer to shared arena */
  res->client = client;         /* CoroNet migration: use client */
  res->status = 200;
  res->content_type = mem_strdup(arena, "text/plain"); /* Phase IRIS-1: Updated */
  res->body = NULL;
  res->body_len = 0;
  res->keep_alive = 1;
  res->headers = NULL;
  res->header_count = 0;
  res->header_capacity = 0;

  return res;
}

// Create and initialize http_context_t
/* Phase IRIS-1: Updated to use mem_pool_t */
static http_context_t *create_http_context(mem_pool_t *arena) {
  if (!arena)
    return NULL;

  http_context_t *context =
      mem_alloc(arena, sizeof(http_context_t)); /* Phase IRIS-1: Updated */
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

/* Phase IRIS-1: Updated to use mem_pool_t */
request_t copy_request_t(mem_pool_t *arena, const request_t *original) {
  request_t copy;
  memset(&copy, 0, sizeof(request_t));

  if (!original || original->count == 0)
    return copy;

  if (arena) {
    // Allocate items array in arena
    copy.capacity = original->capacity;
    copy.count = original->count;
    copy.items = mem_alloc(arena, copy.capacity * sizeof(request_item_t));

    if (!copy.items) {
      copy.capacity = 0;
      copy.count = 0;
      return copy;
    }

    // Copy each item using arena
    for (int i = 0; i < original->count; i++) {
      if (original->items[i].key) {
        copy.items[i].key = mem_strdup(arena, original->items[i].key);
        if (!copy.items[i].key) {
          // Arena allocation failed - clear and return
          memset(&copy, 0, sizeof(request_t));
          return copy;
        }
      } else {
        copy.items[i].key = NULL;
      }

      if (original->items[i].value) {
        copy.items[i].value = mem_strdup(arena, original->items[i].value);
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

  mem_pool_t *arena = req->arena;

  // Copy method
  if (context->method) {
    req->method = mem_strdup(arena, context->method);
    if (!req->method)
      return -1;
  }

  // Copy path
  if (path) {
    req->path = mem_strdup(arena, path);
    if (!req->path)
      return -1;
  }

  // Copy body when this is not a streaming request
  if (!context->stream_mode && context->body && context->body_length > 0) {
    req->body = mem_alloc(arena, context->body_length + 1);
    if (!req->body)
      return -1;
    memcpy(req->body, context->body, context->body_length);
    req->body[context->body_length] = '\0';
    req->body_len = context->body_length;
  }

  req->headers = copy_request_t(arena, &context->headers);
  req->query = copy_request_t(arena, &context->query_params);
  req->params = copy_request_t(arena, &context->url_params);
  req->body_stream = context->stream_mode ? 1 : 0;
  req->body_read_total = 0;

  return 0;
}

int req_is_body_stream(const Req *req) {
  return req ? req->body_stream : 0;
}

size_t req_read_body(Req *req, char *buffer, size_t capacity) {
  iris_connection_ctx_t *conn;

  if (!req || !req->client || !buffer || capacity == 0 || !req->body_stream) {
    return 0;
  }

  conn = (iris_connection_ctx_t *)coro_socket_get_user_data(req->client);
  if (!conn || !conn->request_ctx || !conn->request_streaming_active) {
    return 0;
  }

  while (1) {
    size_t to_copy;

    if (conn->pending_body_chunk_len > conn->pending_body_chunk_offset) {
      to_copy = conn->pending_body_chunk_len - conn->pending_body_chunk_offset;
      if (to_copy > capacity) {
        to_copy = capacity;
      }

      memcpy(buffer, conn->pending_body_chunk + conn->pending_body_chunk_offset, to_copy);
      conn->pending_body_chunk_offset += to_copy;
      req->body_read_total += to_copy;

      if (conn->pending_body_chunk_offset >= conn->pending_body_chunk_len) {
        conn->pending_body_chunk = NULL;
        conn->pending_body_chunk_len = 0;
        conn->pending_body_chunk_offset = 0;
        llhttp_resume(&conn->request_ctx->parser_impl->parser);
        if (conn->parsed_offset == conn->buffer_used) {
          size_t flushed = 0;
          int flush_result = http_context_execute(conn->request_ctx, "", 0, &flushed);
          (void)flushed;
          if (flush_result == 1) {
            conn->request_ctx->message_complete = 1;
          }
        }
      }

      return to_copy;
    }

    if (conn->request_ctx->message_complete) {
      return 0;
    }

    if (conn->parsed_offset < conn->buffer_used) {
      size_t consumed = 0;
      int parse_result = http_context_execute(conn->request_ctx, conn->buffer + conn->parsed_offset,
                                              conn->buffer_used - conn->parsed_offset, &consumed);
      conn->parsed_offset += consumed;

      if (parse_result < 0) {
        return 0;
      }

      if (parse_result == 3) {
        conn->pending_body_chunk = conn->request_ctx->stream_chunk;
        conn->pending_body_chunk_len = conn->request_ctx->stream_chunk_len;
        conn->pending_body_chunk_offset = 0;
        continue;
      }

      if (parse_result == 1) {
        conn->request_ctx->message_complete = 1;
        return 0;
      }
    }

    if (conn->parsed_offset == conn->buffer_used) {
      size_t flushed = 0;
      int flush_result = http_context_execute(conn->request_ctx, "", 0, &flushed);
      (void)flushed;
      if (flush_result == 1) {
        conn->request_ctx->message_complete = 1;
        return 0;
      }
    }

    {
      char *recv_data = NULL;
      size_t recv_len = 0;
      int r = coro_socket_recv(req->client, &recv_data, &recv_len);

      if (r != 0 || !recv_data || recv_len == 0) {
        if (recv_data) {
          coro_socket_free_recv(recv_data);
        }
        return 0;
      }

      if (conn->parsed_offset == conn->buffer_used) {
        conn->parsed_offset = 0;
        conn->buffer_used = 0;
        if (conn->buffer) {
          conn->buffer[0] = '\0';
        }
      }

      if (conn->buffer_used + recv_len + 1 > conn->buffer_capacity) {
        size_t new_capacity = conn->buffer_capacity ? conn->buffer_capacity : 8192;
        char *new_buffer;

        while (new_capacity < conn->buffer_used + recv_len + 1) {
          new_capacity *= 2;
        }

        new_buffer = (char *)realloc(conn->buffer, new_capacity);
        if (!new_buffer) {
          coro_socket_free_recv(recv_data);
          return 0;
        }

        conn->buffer = new_buffer;
        conn->buffer_capacity = new_capacity;
      }

      memcpy(conn->buffer + conn->buffer_used, recv_data, recv_len);
      conn->buffer_used += recv_len;
      conn->buffer[conn->buffer_used] = '\0';
      coro_socket_free_recv(recv_data);
    }
  }
}

// Composes and sends the response (headers + body) using CoroNet send
void reply(Res *res, int status, const char *content_type, const void *body, size_t body_len) {
  if (!res || !res->client) {
    return;
  }

  if (!content_type)
    content_type = "text/plain";
  if (!body)
    body_len = 0;

  // Get current date in HTTP format
  time_t now = time(NULL);
  struct tm *gmt = gmtime(&now);
  char date_str[64];
  strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", gmt);

  tstr_t all_headers = tstr_new();
  if (!all_headers) {
    send_error(res->client, 500);
    return;
  }
  for (int i = 0; i < res->header_count; i++) {
    if (res->headers[i].name && res->headers[i].value) {
      all_headers = tstr_cat_fmt(all_headers, "%s: %s\r\n", res->headers[i].name,
                                 res->headers[i].value);
      if (!all_headers) {
        send_error(res->client, 500);
        return;
      }
    }
  }

  tstr_t response = tstr_new();
  if (!response) {
    tstr_free(all_headers);
    send_error(res->client, 500);
    return;
  }

  response = tstr_cat_fmt(response,
                          "HTTP/1.1 %d\r\n"
                          "Server: Iris Http Server\r\n"
                          "Date: %s\r\n"
                          "%s"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: %s\r\n"
                          "\r\n",
                          status, date_str, all_headers, content_type, body_len,
                          res->keep_alive ? "keep-alive" : "close");
  tstr_free(all_headers);
  if (!response) {
    send_error(res->client, 500);
    return;
  }

  if (body_len > 0 && body) {
    response = tstr_cat_len(response, body, body_len);
    if (!response) {
      send_error(res->client, 500);
      return;
    }
  }

  // Send using CoroNet API
  (void)send_res_data(res, response, tstr_len(response), "response");

  // Free the response buffer immediately since CoroNet copies the data
  tstr_free(response);
}

// Streaming (SSE) support implementation

void reply_stream_start(Res *res, int status) {
  if (!res || !res->client)
    return;

  // Get current date
  time_t now = time(NULL);
  struct tm *gmt = gmtime(&now);
  char date_str[64];
  strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", gmt);

  // Headers for SSE
  // Note: We use Transfer-Encoding: chunked to support streaming properly
  char headers[1024];
  int n = fmt(headers, sizeof(headers),
                         "HTTP/1.1 %d OK\r\n"
                         "Server: Iris Http Server\r\n"
                         "Date: %s\r\n"
                         "Content-Type: text/event-stream\r\n"
                         "Cache-Control: no-cache\r\n"
                         "Connection: keep-alive\r\n"
                         "Transfer-Encoding: chunked\r\n"
                         "X-Accel-Buffering: no\r\n" // Hint for Nginx
                         "\r\n", 
                         status, date_str);

  if (n <= 0 || (size_t)n >= sizeof(headers)) {
    TLOG_ERROR("Failed to format stream start headers");
    res->keep_alive = 0;
    return;
  }

  if (coro_socket_send(res->client, headers, n) != 0) {
    TLOG_ERROR("Failed to send stream start headers");
    res->keep_alive = 0;
  }
}

void reply_stream_chunk(Res *res, const char *data) {
  if (!res || !res->client || !data)
    return;

  // Format the SSE payload: "data: <content>\n\n"
  // Since we are using chunked transfer encoding, we need to wrap this in a chunk.
  
  tstr_t payload = tstr_new();
  if (!payload) {
    TLOG_ERROR("Failed to allocate stream payload buffer");
    res->keep_alive = 0;
    return;
  }
  payload = tstr_cat_fmt(payload, "data: %s\n\n", data);
  if (!payload) {
    TLOG_ERROR("Failed to format stream payload");
    res->keep_alive = 0;
    return;
  }
  size_t payload_len = tstr_len(payload);
  
  // Now create the HTTP chunk
  // Format: <hex_len>\r\n<payload>\r\n
  char hex_len[32];
  int hex_len_size = fmt(hex_len, sizeof(hex_len), "{:x}\r\n", payload_len);
  if (hex_len_size <= 0 || (size_t)hex_len_size >= sizeof(hex_len)) {
    TLOG_ERROR("Failed to format stream chunk size");
    res->keep_alive = 0;
    tstr_free(payload);
    return;
  }
  
  // Send length
  if (coro_socket_send(res->client, hex_len, (size_t)hex_len_size) != 0 ||
      coro_socket_send(res->client, payload, payload_len) != 0 ||
      coro_socket_send(res->client, "\r\n", 2) != 0) {
    TLOG_ERROR("Failed to send stream chunk");
    res->keep_alive = 0;
  }
  
  tstr_free(payload);
}

void reply_stream_end(Res *res) {
  if (!res || !res->client)
    return;
    
  // Send the zero-length chunk to signal end of stream
  // Format: 0\r\n\r\n
  const char *end_chunk = "0\r\n\r\n";
  if (coro_socket_send(res->client, end_chunk, strlen(end_chunk)) != 0) {
    TLOG_ERROR("Failed to send stream end chunk");
    res->keep_alive = 0;
  }
}

// =============================================================================
// File Download Implementation
// =============================================================================

static int send_headers_only(Res *res, int status, const char *content_type,
                             size_t content_length, const char *extra_headers) {
  time_t now = time(NULL);
  struct tm *gmt = gmtime(&now);
  char date_str[64];
  strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", gmt);

  char headers[2048];
  int n = fmt(headers, sizeof(headers),
                         "HTTP/1.1 %d OK\r\n"
                         "Server: Iris Http Server\r\n"
                         "Date: %s\r\n"
                         "Content-Type: %s\r\n"
                         "Content-Length: %zu\r\n"
                         "%s"
                         "Connection: %s\r\n"
                         "\r\n",
                         status, date_str, content_type, content_length,
                         extra_headers ? extra_headers : "",
                         res->keep_alive ? "keep-alive" : "close");

  if (n <= 0 || (size_t)n >= sizeof(headers)) {
    TLOG_ERROR("Failed to format response headers");
    res->keep_alive = 0;
    return -1;
  }

  return send_res_data(res, headers, (size_t)n, "response headers");
}

int reply_file(Res *res, int status, const char *content_type, const char *file_path) {
  if (!res || !res->client || !file_path)
    return -1;

  FILE *fp = fopen(file_path, "rb");
  if (!fp)
    return -1;

  fseek(fp, 0, SEEK_END);
  long file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  if (file_size < 0) {
    fclose(fp);
    return -1;
  }

  char *data = malloc((size_t)file_size);
  if (!data) {
    fclose(fp);
    return -1;
  }

  size_t read = fread(data, 1, (size_t)file_size, fp);
  fclose(fp);

  if (read != (size_t)file_size) {
    free(data);
    return -1;
  }

  if (send_headers_only(res, status, content_type, (size_t)file_size, NULL) != 0 ||
      send_res_data(res, data, (size_t)file_size, "file response body") != 0) {
    free(data);
    return -1;
  }
  free(data);
  return 0;
}

int reply_download(Res *res, const char *file_path, const char *download_name) {
  if (!res || !res->client || !file_path)
    return -1;

  // Extract filename from path if download_name not provided
  const char *filename = download_name;
  if (!filename) {
    filename = strrchr(file_path, '/');
    if (!filename) filename = strrchr(file_path, '\\');
    filename = filename ? filename + 1 : file_path;
  }

  FILE *fp = fopen(file_path, "rb");
  if (!fp)
    return -1;

  fseek(fp, 0, SEEK_END);
  long file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  if (file_size < 0) {
    fclose(fp);
    return -1;
  }

  char *data = malloc((size_t)file_size);
  if (!data) {
    fclose(fp);
    return -1;
  }

  size_t read = fread(data, 1, (size_t)file_size, fp);
  fclose(fp);

  if (read != (size_t)file_size) {
    free(data);
    return -1;
  }

  char extra[512];
  fmt(extra, sizeof(extra), "Content-Disposition: attachment; filename=\"{}\"\r\n", filename);

  if (send_headers_only(res, 200, "application/octet-stream", (size_t)file_size, extra) != 0 ||
      send_res_data(res, data, (size_t)file_size, "attachment response body") != 0) {
    free(data);
    return -1;
  }
  free(data);
  return 0;
}

static int reply_chunked_start_impl(Res *res, int status, const char *content_type) {
  if (!res || !res->client)
    return -1;

  time_t now = time(NULL);
  struct tm *gmt = gmtime(&now);
  char date_str[64];
  strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", gmt);

  char headers[1024];
  int n = fmt(headers, sizeof(headers),
                         "HTTP/1.1 %d OK\r\n"
                         "Server: Iris Http Server\r\n"
                         "Date: %s\r\n"
                         "Content-Type: %s\r\n"
                         "Transfer-Encoding: chunked\r\n"
                         "Connection: %s\r\n"
                         "\r\n",
                         status, date_str, content_type,
                         res->keep_alive ? "keep-alive" : "close");

  if (n <= 0 || (size_t)n >= sizeof(headers)) {
    TLOG_ERROR("Failed to format chunked response headers");
    res->keep_alive = 0;
    return -1;
  }

  return send_res_data(res, headers, (size_t)n, "chunked response headers");
}

void reply_chunked_start(Res *res, int status, const char *content_type) {
  (void)reply_chunked_start_impl(res, status, content_type);
}

static int reply_chunked_write_impl(Res *res, const void *data, size_t len) {
  if (!res || !res->client || !data || len == 0)
    return -1;

  char hex_len[32];
  int n = fmt(hex_len, sizeof(hex_len), "{:x}\r\n", len);
  if (n <= 0 || (size_t)n >= sizeof(hex_len)) {
    TLOG_ERROR("Failed to format chunked response size");
    res->keep_alive = 0;
    return -1;
  }

  if (send_res_data(res, hex_len, (size_t)n, "chunked size") != 0 ||
      send_res_data(res, data, len, "chunked body") != 0 ||
      send_res_data(res, "\r\n", 2, "chunked terminator") != 0) {
    return -1;
  }

  return 0;
}

void reply_chunked_write(Res *res, const void *data, size_t len) {
  (void)reply_chunked_write_impl(res, data, len);
}

static int reply_chunked_end_impl(Res *res) {
  if (!res || !res->client)
    return -1;

  return send_res_data(res, "0\r\n\r\n", 5, "chunked end");
}

void reply_chunked_end(Res *res) {
  (void)reply_chunked_end_impl(res);
}

#define DEFAULT_CHUNK_SIZE (64 * 1024)

int reply_file_chunked(Res *res, int status, const char *content_type,
                       const char *file_path, size_t chunk_size) {
  if (!res || !res->client || !file_path)
    return -1;

  FILE *fp = fopen(file_path, "rb");
  if (!fp)
    return -1;

  if (chunk_size == 0)
    chunk_size = DEFAULT_CHUNK_SIZE;

  char *buffer = malloc(chunk_size);
  if (!buffer) {
    fclose(fp);
    return -1;
  }

  if (reply_chunked_start_impl(res, status, content_type) != 0) {
    free(buffer);
    fclose(fp);
    return -1;
  }

  size_t bytes_read;
  while ((bytes_read = fread(buffer, 1, chunk_size, fp)) > 0) {
    if (reply_chunked_write_impl(res, buffer, bytes_read) != 0) {
      free(buffer);
      fclose(fp);
      return -1;
    }
  }

  if (ferror(fp)) {
    free(buffer);
    fclose(fp);
    return -1;
  }

  if (reply_chunked_end_impl(res) != 0) {
    free(buffer);
    fclose(fp);
    return -1;
  }

  free(buffer);
  fclose(fp);
  return 0;
}

// Validates all cookies in the Cookie header
static iris_security_result_t validate_request_cookies(http_context_t *ctx, const iris_security_limits_t *limits) {
  if (!ctx) {
    return IRIS_SECURITY_ERROR_NULL_POINTER;
  }

  // Find the Cookie header
  const char *cookie_header = NULL;
  for (int i = 0; i < ctx->headers.count; i++) {
    if (ctx->headers.items[i].key && tstr_casecmp(ctx->headers.items[i].key, "Cookie") == 0) {
      cookie_header = ctx->headers.items[i].value;
      break;
    }
  }

  if (!cookie_header) {
    return IRIS_SECURITY_OK;
  }

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
      TLOG_ERROR("Security: Invalid cookie name in request: {} (error: {})", name,
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
      TLOG_ERROR("Security: Invalid cookie value for '{}' (error: {})", name,
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

int iris_app_execute_parsed(iris_app_t *app, coro_socket_t *client, mem_pool_t *arena,
                            http_context_t *ctx) {
  Req *req = NULL;
  Res *res = NULL;
  tokenized_path_t tokenized_path = {0};
  int error_code = 0;
  int should_close = 1;
  bool send_error_response = false;
  bool send_404_response = false;

  if (!app || !client || !arena || !ctx) {
    if (client) {
      send_error(client, 400);
    }
    return 1;
  }

  req = create_req(arena, app, client);
  res = create_res(arena, client);

  if (!req || !res) {
    error_code = 500;
    send_error_response = true;
    goto cleanup;
  }

  // Extract path and query
  char *path = NULL;
  char *query = NULL;
  if (extract_path_and_query(arena, ctx->url, &path, &query) != 0) {
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
  const iris_security_limits_t *limits = &app->security_limits;
  iris_security_result_t path_validation = iris_validate_url_path(path, limits->max_url_length);
  if (path_validation != IRIS_SECURITY_OK) {
    if (path_validation == IRIS_SECURITY_ERROR_MALICIOUS_CONTENT) {
      // Log potential attack attempt
      TLOG_ERROR("Security: Malicious URL path detected: {}", path);
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
  iris_security_result_t cookie_validation = validate_request_cookies(ctx, limits);
  if (cookie_validation != IRIS_SECURITY_OK) {
    if (cookie_validation == IRIS_SECURITY_ERROR_MALICIOUS_CONTENT) {
      // Log potential attack attempt
      TLOG_ERROR("Security: Malicious cookie content detected\n");
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
  parse_query(arena, query, &ctx->query_params);
  res->keep_alive = ctx->keep_alive;

  // Handle CORS preflight
  if (cors_handle_preflight(app->cors_opts, ctx, res)) {
    reply(res, res->status, res->content_type, res->body, res->body_len);
    should_close = !res->keep_alive;
    goto cleanup;
  }

  // Route matching validation
  if (!app->route_trie || !ctx->method) {
    TLOG_ERROR("Missing route trie ({}) or method ({})", (void *)app->route_trie,
               ctx->method ? ctx->method : "NULL");
    cors_add_headers(app->cors_opts, ctx, res);
    send_404_response = true;
    goto cleanup;
  }

  // Tokenize path
  if (tokenize_path(arena, path, &tokenized_path) != 0) {
    error_code = 500;
    send_error_response = true;
    goto cleanup;
  }

  // Route matching
  route_match_t match;
  if (!route_trie_match(app->route_trie, ctx->method, &tokenized_path, &match)) {
    cors_add_headers(app->cors_opts, ctx, res);
    send_404_response = true;
    goto cleanup;
  }

  if (extract_url_params(arena, &match, &ctx->url_params) != 0) {
    error_code = 500;
    send_error_response = true;
    goto cleanup;
  }

  if (populate_req_from_context(req, ctx, path) != 0) {
    error_code = 500;
    send_error_response = true;
    goto cleanup;
  }

  req->body_stream = match.stream_body ? 1 : req->body_stream;

  if (!match.handler) {
    error_code = 500;
    send_error_response = true;
    goto cleanup;
  }

  // Success path - call handler
  cors_add_headers(app->cors_opts, ctx, res);

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
    send_error(client, error_code);
  } else if (send_404_response) {
    const char *not_found_msg = "404 Not Found";
    reply(res, 404, "text/plain", not_found_msg, strlen(not_found_msg));
    should_close = !res->keep_alive;
  }

  return should_close;
}

int iris_app_route_uses_stream(iris_app_t *app, mem_pool_t *arena, http_context_t *ctx) {
  char *url_copy;
  char *path = NULL;
  char *query = NULL;
  tokenized_path_t tokenized_path = {0};
  route_match_t match;

  if (!app || !arena || !ctx || !ctx->url || !ctx->method || !app->route_trie) {
    return -1;
  }

  url_copy = mem_strdup(arena, ctx->url);
  if (!url_copy) {
    return -1;
  }

  if (extract_path_and_query(arena, url_copy, &path, &query) != 0 || !path) {
    return -1;
  }

  if (tokenize_path(arena, path, &tokenized_path) != 0) {
    return -1;
  }

  if (!route_trie_match(app->route_trie, ctx->method, &tokenized_path, &match)) {
    return 0;
  }

  return match.stream_body ? 1 : 0;
}

// Main router function
int iris_app_execute(iris_app_t *app, coro_socket_t *client, const char *request_data,
                     size_t request_len) {
  mem_pool_t arena;
  http_context_t *ctx;
  size_t consumed = 0;
  int parse_result;
  int should_close;

  if (!app || !client || !request_data || request_len == 0) {
    if (client)
      send_error(client, 400);
    return 1;
  }

  if (mem_init(&arena, 8192) != 0) {
    send_error(client, 500);
    return 1;
  }

  ctx = create_http_context(&arena);
  if (!ctx) {
    mem_destroy(&arena);
    send_error(client, 500);
    return 1;
  }

  parse_result = http_context_execute(ctx, request_data, request_len, &consumed);
  if (parse_result <= 0 || consumed != request_len) {
    mem_destroy(&arena);
    send_error(client, (parse_result == -413) ? 413 : 400);
    return 1;
  }

  should_close = iris_app_execute_parsed(app, client, &arena, ctx);
  mem_destroy(&arena);
  return should_close;
}

int router(coro_socket_t *client, const char *request_data, size_t request_len) {
  return iris_app_execute(iris_app_default(), client, request_data, request_len);
}

// Adds a header
void set_header(Res *res, const char *name, const char *value) {
  if (!res || !name || !value) {
    TLOG_ERROR("Error: Invalid argument(s) to set_header\n");
    return;
  }

  if (res->header_count >= res->header_capacity) {
    int new_cap = res->header_capacity ? res->header_capacity * 2 : 8;
    http_header_t *tmp;

    if (res->arena) {
      // Arena-based allocation
      /* Phase IRIS-1: turbo_arena doesn't have realloc, so alloc + memcpy */
      tmp = mem_alloc(res->arena, new_cap * sizeof(http_header_t));
      if (tmp && res->headers && res->header_capacity > 0) {
        memcpy(tmp, res->headers, res->header_capacity * sizeof(http_header_t));
      }
    } else {
      // Malloc-based allocation
      tmp = realloc(res->headers, new_cap * sizeof(http_header_t));
    }

    if (!tmp) {
      TLOG_ERROR("Error: Failed to realloc headers array\n");
      return;
    }

    res->headers = tmp;
    res->header_capacity = new_cap;
  }

  if (res->arena) {
    // Arena-based string allocation
    res->headers[res->header_count].name = mem_strdup(res->arena, name);
    res->headers[res->header_count].value = mem_strdup(res->arena, value);
  } else {
    // Malloc-based string allocation
    res->headers[res->header_count].name = strdup(name);
    res->headers[res->header_count].value = strdup(value);
  }

  if (!res->headers[res->header_count].name || !res->headers[res->header_count].value) {
    TLOG_ERROR("Error: Failed to allocate memory for header strings\n");
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
  copy->client = original->client;
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
  copy->app = original->app;
  copy->arena = NULL;
  copy->client = original->client; /* CoroNet migration: use client */
  copy->body_len = original->body_len;
  copy->body_stream = original->body_stream;
  copy->body_read_total = original->body_read_total;

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

/* Phase IRIS-1: Updated to use mem_pool_t */
Req *arena_copy_req(mem_pool_t *target_arena, const Req *original) {
  if (!original || !target_arena)
    return NULL;

  // Allocate on target arena
  Req *copy = mem_alloc(target_arena, sizeof(Req));
  if (!copy)
    return NULL;

  // Copy primitive fields
  copy->app = original->app;
  copy->arena = target_arena;
  copy->client = original->client; /* CoroNet migration: use client */
  copy->body_len = original->body_len;
  copy->body_stream = original->body_stream;
  copy->body_read_total = original->body_read_total;

  // Deep copy strings using target arena
  if (original->method)
    copy->method = mem_strdup(target_arena, original->method);

  if (original->path)
    copy->path = mem_strdup(target_arena, original->path);

  if (original->body && original->body_len > 0) {
    copy->body = mem_alloc(target_arena, original->body_len + 1);
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
  copy->security = mem_alloc(target_arena, sizeof(iris_security_context_t));
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

/* Phase IRIS-1: Updated to use mem_pool_t */
Res *arena_copy_res(mem_pool_t *target_arena, const Res *original) {
  if (!original || !target_arena)
    return NULL;

  // Allocate on target arena
  Res *copy = mem_alloc(target_arena, sizeof(Res));
  if (!copy)
    return NULL;

  // Copy primitive fields
  *copy = *original;
  copy->arena = target_arena;
  copy->client = original->client;

  if (original->content_type)
    copy->content_type = mem_strdup(target_arena, original->content_type);

  // Copy headers array in target arena
  if (original->header_capacity > 0) {
    copy->headers =
        mem_alloc(target_arena, original->header_capacity * sizeof(http_header_t));

    for (int i = 0; i < original->header_count; ++i) {
      if (original->headers[i].name)
        copy->headers[i].name = mem_strdup(target_arena, original->headers[i].name);
      if (original->headers[i].value)
        copy->headers[i].value = mem_strdup(target_arena, original->headers[i].value);
    }
  }

  return copy;
}
