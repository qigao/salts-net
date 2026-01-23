// clang-format off
// Include order is CRITICAL - do not reorder!
#include "turbo_async_client.h"
#include <llhttp.h>
#include "http_client_async.h"
#include <cjwt/cjwt.h>
#include "turbo_parser.h"
// clang-format on
#include "base64_utils.h"
#include "arena_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stb_sprintf.h>
#include <time.h>

#ifdef _WIN32
  #define strdup _strdup
  #define strcasecmp _stricmp
  #define strncasecmp _strnicmp
#else
  #include <strings.h>
#endif

/* Header entry structure */
typedef struct header_entry_s {
  char *name;
  char *value;
  struct header_entry_s *next;
} header_entry_t;

/* URI parsing using uri_parser.h */

/* Parser context for llhttp */
typedef struct {
  http_async_response_t *response;
  int headers_complete;
  char *current_header_field;
  char *current_header_value;
} parser_context_t;

/* Request state */
typedef enum {
  REQUEST_STATE_PENDING,
  REQUEST_STATE_CONNECTING,
  REQUEST_STATE_SENDING,
  REQUEST_STATE_RECEIVING,
  REQUEST_STATE_COMPLETE,
  REQUEST_STATE_CANCELLED,
  REQUEST_STATE_ERROR
} request_state_t;

/* Async request structure */
struct http_async_request_s {
  http_async_client_t *client;
  http_method_t method;
  char *url;
  char **headers;
  int header_count;
  char *body;
  size_t body_len;

  http_async_response_cb callback;
  void *user_data;

  http_async_progress_cb progress_callback;
  void *progress_user_data;

  request_state_t state;
  http_async_response_t *response;

  char *receive_buffer;
  size_t receive_buffer_size;
  size_t receive_buffer_used;

  uri_t *uri;
  llhttp_t parser;
  llhttp_settings_t parser_settings;  /* Must persist for llhttp_execute! */
  parser_context_t parser_ctx;

  int redirect_count;

  struct http_async_request_s *next;
  struct http_async_request_s *prev;

  /* Phase HTTP-1 Optimization: Request arena for all request-lifetime allocations
   * All request data (url, headers, body, receive_buffer, response) allocated from here
   * Single turbo_arena_free() cleans up everything */
  turbo_arena_t request_arena;
};

/* Cookie structure */
typedef struct http_async_cookie_s {
  char *name;
  char *value;
  char *domain;
  char *path;
  int secure;
  int http_only;
  struct http_async_cookie_s *next;
} http_async_cookie_t;

/* Cookie jar structure */
struct http_async_cookie_jar_s {
  http_async_cookie_t *cookies;
  int count;
};

/* URL parameters structure */
struct http_async_params_s {
  struct param_entry {
    char *key;
    char *value;
    struct param_entry *next;
  } *head;
  int count;
};

/* Multipart form part */
typedef struct http_async_multipart_part_s {
  char *name;
  char *filename;
  char *content_type;
  char *value;
  void *data;
  size_t data_len;
  int is_file;
  struct http_async_multipart_part_s *next;
} http_async_multipart_part_t;

/* Multipart form structure */
struct http_async_multipart_form_s {
  http_async_multipart_part_t *parts;
  char boundary[48];
  int part_count;
};

/* Interceptor list node */
typedef struct http_async_interceptor_node_s {
  union {
    http_async_request_interceptor_t request;
    http_async_response_interceptor_t response;
  } callback;
  void *user_data;
  struct http_async_interceptor_node_s *next;
} http_async_interceptor_node_t;

/* Async client structure */
struct http_async_client_s {
  async_client_t *client;

  int timeout_ms;
  int connect_timeout_ms;
  char *user_agent;
  int follow_redirects;
  int max_redirects;
  char *base_url;

  header_entry_t *default_headers;
  int default_header_count;

  char *auth_header;

  http_async_cookie_jar_t *cookie_jar;

  http_async_interceptor_node_t *request_interceptors;
  http_async_interceptor_node_t *response_interceptors;

  http_async_retry_policy_t retry_policy;
  int has_retry_policy;

  int compression_enabled;

  http_async_rate_limit_t rate_limit;
  int has_rate_limit;
  double last_request_time;
  int tokens;

  http_async_request_t *active_requests;

  http_async_client_stats_t stats;

  /* Phase HTTP-1 Optimization: Client arena for all client-lifetime allocations
   * All client config (user_agent, base_url, auth_header, default_headers) allocated from here
   * Single turbo_arena_free() cleans up everything */
  turbo_arena_t client_arena;
};

/* Forward declarations */
static void async_event_handler(async_client_t *client, const async_client_event_t *event,
                                void *user_data);
static void process_response_data(http_async_request_t *request, const char *data, size_t len);
static void initiate_request(http_async_request_t *request);
static void send_http_request(http_async_request_t *request);
static void complete_request(http_async_request_t *request);
static void remove_request(http_async_client_t *client, http_async_request_t *request);
static const char *method_to_string(http_method_t method);
static char *get_header_value(const char *headers, const char *header_name);
static char *build_full_url(http_async_client_t *client, const char *url, turbo_arena_t *arena);

/* Client lifecycle */
http_async_client_t *http_async_client_create(void) {
  http_async_client_t *client = calloc(1, sizeof(http_async_client_t));
  if (!client)
    return NULL;

  /* Phase HTTP-1: Initialize client arena (4KB initial - enough for client config)
   * All client config strings allocated from here */
  if (turbo_arena_init(&client->client_arena, 4096) != 0) {
    free(client);
    return NULL;
  }

  /* Use URL-based transport determination */
  client->client = async_client_create(async_event_handler, client);
  if (!client->client) {
    turbo_arena_free(&client->client_arena);
    free(client);
    return NULL;
  }

  client->timeout_ms = 30000;
  client->connect_timeout_ms = 10000;

  /* Phase HTTP-1: Allocate user_agent from arena */
  client->user_agent = turbo_arena_strdup(&client->client_arena, "TurboHTTP-Async/1.0");

  client->follow_redirects = 1;
  client->max_redirects = 10;

  return client;
}

void http_async_client_destroy(http_async_client_t *client) {
  if (!client)
    return;

  if (client->client) {
    async_client_destroy(client->client);
  }

  /* Phase HTTP-1 Optimization: Single arena_free() replaces 20+ individual free() calls!
   * This frees: user_agent, base_url, auth_header, all default_headers (names + values + structs)
   * Before: 3 + (N × 3) free() calls for headers = 20+ free() calls
   * After: ONE call frees everything! */
  turbo_arena_free(&client->client_arena);

  /* Note: cookie_jar is not freed here - user must manage it separately */

  /* Free interceptors (not in arena - separate lifecycle) */
  http_async_interceptor_node_t *node = client->request_interceptors;
  while (node) {
    http_async_interceptor_node_t *next = node->next;
    free(node);
    node = next;
  }

  node = client->response_interceptors;
  while (node) {
    http_async_interceptor_node_t *next = node->next;
    free(node);
    node = next;
  }

  free(client);
}

/* Configuration */
void http_async_client_set_timeout(http_async_client_t *client, int timeout_ms) {
  if (client) {
    client->timeout_ms = timeout_ms;
    async_client_set_operation_timeout(client->client, timeout_ms);
  }
}

void http_async_client_set_connect_timeout(http_async_client_t *client, int timeout_ms) {
  if (client) {
    client->connect_timeout_ms = timeout_ms;
    async_client_set_connect_timeout(client->client, timeout_ms);
  }
}

void http_async_client_set_user_agent(http_async_client_t *client, const char *user_agent) {
  if (!client)
    return;
  /* Phase HTTP-1: No need to free old value - arena owns it
   * Just allocate new value from arena */
  client->user_agent = turbo_arena_strdup(&client->client_arena, user_agent);
}

void http_async_client_follow_redirects(http_async_client_t *client, int follow) {
  if (client)
    client->follow_redirects = follow;
}

void http_async_client_set_max_redirects(http_async_client_t *client, int max_redirects) {
  if (client && max_redirects >= 0)
    client->max_redirects = max_redirects;
}

void http_async_client_set_base_url(http_async_client_t *client, const char *base_url) {
  if (!client)
    return;
  /* Phase HTTP-1: No need to free old value - arena owns it */
  client->base_url = base_url ? turbo_arena_strdup(&client->client_arena, base_url) : NULL;
}

/* Default headers */
void http_async_client_set_default_header(http_async_client_t *client, const char *name,
                                          const char *value) {
  if (!client || !name || !value)
    return;

  header_entry_t *entry = client->default_headers;
  while (entry) {
    if (strcasecmp(entry->name, name) == 0) {
      /* Phase HTTP-1: No need to free old value - arena owns it */
      entry->value = turbo_arena_strdup(&client->client_arena, value);
      return;
    }
    entry = entry->next;
  }

  /* Phase HTTP-1: Allocate entry from arena */
  entry = (header_entry_t *)turbo_arena_alloc(&client->client_arena, sizeof(header_entry_t));
  if (!entry)
    return;

  entry->name = turbo_arena_strdup(&client->client_arena, name);
  entry->value = turbo_arena_strdup(&client->client_arena, value);
  entry->next = client->default_headers;
  client->default_headers = entry;
  client->default_header_count++;
}

void http_async_client_clear_default_headers(http_async_client_t *client) {
  if (!client)
    return;

  /* Phase HTTP-1: No need to free entries - arena owns them
   * Just reset the list */
  client->default_headers = NULL;
  client->default_header_count = 0;
}

void http_async_client_set_basic_auth(http_async_client_t *client, const char *username,
                                      const char *password) {
  if (!client || !username || !password)
    return;

  size_t creds_len = strlen(username) + strlen(password) + 2;
  char *credentials = malloc(creds_len);
  stbsp_snprintf(credentials, (int)creds_len, "%s:%s", username, password);

  char *encoded = NULL;
  if (tn_base64_encode((const uint8_t *)credentials, strlen(credentials), &encoded) != 0) {
    free(credentials);
    return;
  }
  free(credentials);

  if (!encoded)
    return;

  /* Phase HTTP-1: No need to free old auth_header - arena owns it
   * Allocate new auth_header from arena */
  size_t header_len = strlen("Authorization: Basic ") + strlen(encoded) + 1;
  client->auth_header = (char *)turbo_arena_alloc(&client->client_arena, header_len);
  stbsp_snprintf(client->auth_header, (int)header_len, "Authorization: Basic %s", encoded);
  free(encoded);
}

void http_async_client_set_bearer_token(http_async_client_t *client, const char *token) {
  if (!client || !token)
    return;

  /* Phase HTTP-1: No need to free old auth_header - arena owns it */
  size_t header_len = strlen("Authorization: Bearer ") + strlen(token) + 1;
  client->auth_header = (char *)turbo_arena_alloc(&client->client_arena, header_len);
  stbsp_snprintf(client->auth_header, (int)header_len, "Authorization: Bearer %s", token);
}

void http_async_client_set_jwt_auth(http_async_client_t *client, const char *secret, const char *claims_json) {
  if (!client || !secret || !claims_json)
    return;

  cJSON *private_claims = cJSON_Parse(claims_json);
  if (!private_claims) {
    // TLOG not used in async client yet? Let's check.
    // Actually async client uses printf for now based on previous view.
    printf("[ERROR] Failed to parse JWT claims JSON\n");
    return;
  }

  cjwt_t jwt = {0};
  jwt.header.alg = alg_hs256;
  jwt.private_claims = private_claims;

  char *token = NULL;
  cjwt_code_t rv = cjwt_encode(&jwt, (const uint8_t *)secret, strlen(secret), &token);
  cJSON_Delete(private_claims);

  if (rv != CJWTE_OK) {
    printf("[ERROR] Failed to encode JWT: %d\n", rv);
    return;
  }

  http_async_client_set_bearer_token(client, token);
  free(token);
}


void http_async_client_clear_auth(http_async_client_t *client) {
  if (!client)
    return;
  /* Phase HTTP-1: No need to free - arena owns it */
  client->auth_header = NULL;
}

/* Async request API - full implementation */
http_async_request_t *http_async_request(http_async_client_t *client, http_method_t method,
                                         const char *url, const char **headers, int header_count,
                                         const char *body, size_t body_len,
                                         http_async_response_cb callback, void *user_data) {
  if (!client || !url || !callback)
    return NULL;

  http_async_request_t *request = calloc(1, sizeof(http_async_request_t));
  if (!request)
    return NULL;

  /* Phase HTTP-1: Initialize request arena (8KB initial - enough for typical request)
   * All request data (url, headers, body, receive_buffer, response) allocated from here */
  if (turbo_arena_init(&request->request_arena, 8192) != 0) {
    free(request);
    return NULL;
  }

  request->client = client;
  request->method = method;

  /* Phase HTTP-1: Build full URL and allocate from arena */
  char *full_url = build_full_url(client, url, &request->request_arena);
  request->url = full_url ? full_url : turbo_arena_strdup(&request->request_arena, url);

  request->callback = callback;
  request->user_data = user_data;
  request->state = REQUEST_STATE_PENDING;
  request->redirect_count = 0;

  /* Phase HTTP-1: Copy headers - allocate array and strings from arena */
  if (headers && header_count > 0) {
    request->headers = (char **)turbo_arena_alloc(&request->request_arena,
                                                   sizeof(char *) * header_count);
    request->header_count = header_count;
    for (int i = 0; i < header_count; i++) {
      request->headers[i] = turbo_arena_strdup(&request->request_arena, headers[i]);
    }
  }

  /* Phase HTTP-1: Copy body - allocate from arena */
  if (body && body_len > 0) {
    request->body = (char *)turbo_arena_alloc(&request->request_arena, body_len);
    memcpy(request->body, body, body_len);
    request->body_len = body_len;
  }

  /* Phase HTTP-1: Allocate response from arena */
  request->response = (http_async_response_t *)turbo_arena_alloc(&request->request_arena,
                                                                  sizeof(http_async_response_t));
  memset(request->response, 0, sizeof(http_async_response_t));

  /* Phase HTTP-1: Initialize receive buffer from arena */
  request->receive_buffer_size = 65536;
  request->receive_buffer = (char *)turbo_arena_alloc(&request->request_arena,
                                                       request->receive_buffer_size);
  request->receive_buffer_used = 0;

  /* Add to active requests list */
  request->next = client->active_requests;
  request->prev = NULL;
  if (client->active_requests) {
    client->active_requests->prev = request;
  }
  client->active_requests = request;

  client->stats.total_requests++;
  client->stats.active_requests++;

  /* Initiate the request */
  initiate_request(request);

  return request;
}

http_async_request_t *http_async_get(http_async_client_t *client, const char *url,
                                     http_async_response_cb callback, void *user_data) {
  return http_async_request(client, HTTP_GET, url, NULL, 0, NULL, 0, callback, user_data);
}

http_async_request_t *http_async_post(http_async_client_t *client, const char *url,
                                      const char *body, size_t body_len,
                                      http_async_response_cb callback, void *user_data) {
  return http_async_request(client, HTTP_POST, url, NULL, 0, body, body_len, callback, user_data);
}

void http_async_request_cancel(http_async_request_t *request) {
  if (!request)
    return;
  request->state = REQUEST_STATE_CANCELLED;
}

void http_async_request_set_progress_callback(http_async_request_t *request,
                                              http_async_progress_cb callback, void *user_data) {
  if (!request)
    return;
  request->progress_callback = callback;
  request->progress_user_data = user_data;
}

/* Response helpers */
void http_async_response_free(http_async_response_t *response) {
  if (!response)
    return;
  free(response->headers);
  free(response->body);
  free(response->error);
  free(response);
}

char *http_async_response_get_header(http_async_response_t *response, const char *name) {
  if (!response || !name || !response->headers)
    return NULL;

  return get_header_value(response->headers, name);
}

int http_async_response_is_json(http_async_response_t *response) {
  char *content_type = http_async_response_get_header(response, "Content-Type");
  if (!content_type)
    return 0;

  int is_json = (strstr(content_type, "application/json") != NULL ||
                 strstr(content_type, "application/javascript") != NULL ||
                 strstr(content_type, "text/json") != NULL);
  free(content_type);
  return is_json;
}

/* Statistics */
void http_async_client_get_stats(http_async_client_t *client, http_async_client_stats_t *stats) {
  if (!client || !stats)
    return;
  *stats = client->stats;
}

void http_async_client_reset_stats(http_async_client_t *client) {
  if (!client)
    return;
  memset(&client->stats, 0, sizeof(http_async_client_stats_t));
}

/* llhttp callbacks - extract HTTP response data */
static int on_status(llhttp_t *parser, const char *at, size_t length) {
  (void)at;
  (void)length;
  parser_context_t *ctx = (parser_context_t *)parser->data;
  if (ctx && ctx->response) {
    ctx->response->status_code = (int)llhttp_get_status_code(parser);
  }
  return 0;
}

static int on_header_field(llhttp_t *parser, const char *at, size_t length) {
  parser_context_t *ctx = (parser_context_t *)parser->data;
  if (!ctx)
    return 0;

  /* Free previous incomplete header field */
  free(ctx->current_header_field);
  ctx->current_header_field = malloc(length + 1);
  if (ctx->current_header_field) {
    memcpy(ctx->current_header_field, at, length);
    ctx->current_header_field[length] = '\0';
  }
  return 0;
}

static int on_header_value(llhttp_t *parser, const char *at, size_t length) {
  parser_context_t *ctx = (parser_context_t *)parser->data;
  if (!ctx)
    return 0;

  /* Free previous incomplete header value */
  free(ctx->current_header_value);
  ctx->current_header_value = malloc(length + 1);
  if (ctx->current_header_value) {
    memcpy(ctx->current_header_value, at, length);
    ctx->current_header_value[length] = '\0';
  }
  return 0;
}

static int on_headers_complete(llhttp_t *parser) {
  parser_context_t *ctx = (parser_context_t *)parser->data;
  if (ctx) {
    ctx->headers_complete = 1;
    if (ctx->response) {
      ctx->response->status_code = (int)llhttp_get_status_code(parser);
    }
  }
  return 0;
}

static int on_body(llhttp_t *parser, const char *at, size_t length) {
  parser_context_t *ctx = (parser_context_t *)parser->data;
  if (!ctx || !ctx->response)
    return 0;

  http_async_response_t *response = ctx->response;

  /* Append body data */
  if (!response->body) {
    response->body = malloc(length + 1);
    if (response->body) {
      memcpy(response->body, at, length);
      response->body[length] = '\0';
      response->body_len = length;
    }
  } else {
    char *new_body = realloc(response->body, response->body_len + length + 1);
    if (new_body) {
      memcpy(new_body + response->body_len, at, length);
      response->body_len += length;
      new_body[response->body_len] = '\0';
      response->body = new_body;
    }
  }
  return 0;
}

static int on_message_complete(llhttp_t *parser) {
  parser_context_t *ctx = (parser_context_t *)parser->data;
  if (ctx) {
    ctx->headers_complete = 2; /* Use 2 to indicate message complete */
  }
  return 0;
}

/* Event handler */
static void async_event_handler(async_client_t *client, const async_client_event_t *event,
                                void *user_data) {
  http_async_client_t *http_client = (http_async_client_t *)user_data;

  /* Find the active request for this connection */
  http_async_request_t *request = http_client->active_requests;

  printf("[DEBUG] async_event_handler called: type=%d\n", event->type);

  switch (event->type) {
  case ASYNC_CLIENT_EVENT_CONNECTED:
    printf("[DEBUG] EVENT_CONNECTED received\n");
    /* Connection established - send HTTP request */
    if (request && request->state == REQUEST_STATE_CONNECTING) {
      printf("[DEBUG] Sending HTTP request\n");
      send_http_request(request);
    } else {
      printf("[DEBUG] WARNING: CONNECTED but no request or wrong state (request=%p, state=%d)\n",
             (void*)request, request ? request->state : -1);
    }
    break;

  case ASYNC_CLIENT_EVENT_DATA:
    printf("[DEBUG] EVENT_DATA received: %zu bytes\n", event->length);
    /* Received data - parse HTTP response */
    if (request && event->data && event->length > 0) {
      process_response_data(request, event->data, event->length);
    }
    break;

  case ASYNC_CLIENT_EVENT_CLOSED:
    printf("[DEBUG] EVENT_CLOSED received\n");
    /* Connection closed */
    if (request && request->state == REQUEST_STATE_RECEIVING) {
      complete_request(request);
    }
    break;

  case ASYNC_CLIENT_EVENT_ERROR:
    printf("[DEBUG] EVENT_ERROR received: status=%d, message=%s\n",
           event->status, event->message ? event->message : "NULL");
    /* Error occurred */
    if (request) {
      request->state = REQUEST_STATE_ERROR;
      if (!request->response->error) {
        /* Phase HTTP-1: Allocate error from arena */
        const char *error_msg = event->message ? event->message : "Connection error";
        request->response->error = turbo_arena_strdup(&request->request_arena, error_msg);
        request->response->error_code = HTTP_ASYNC_ERROR_CONNECTION_FAILED;
      }
      complete_request(request);
    }
    break;
  }
}

static void process_response_data(http_async_request_t *request, const char *data, size_t len) {
  if (!request || !data || len == 0)
    return;

  printf("[DEBUG] process_response_data: %zu bytes\n", len);

  /* Accumulate data in receive buffer */
  if (request->receive_buffer_used + len > request->receive_buffer_size) {
    /* Phase HTTP-1: Arena growth pattern (like MQTT Phase 6/7)
     * Cannot use realloc() on arena memory - allocate new buffer and copy */
    size_t new_size = (request->receive_buffer_used + len) * 2;
    char *new_buffer = (char *)turbo_arena_alloc(&request->request_arena, new_size);
    if (!new_buffer) {
      /* Arena allocation failed - cannot continue */
      request->response->error = (char *)turbo_arena_alloc(&request->request_arena, 64);
      if (request->response->error) {
        stbsp_snprintf(request->response->error, 64, "Arena allocation failed for receive buffer");
      }
      request->state = REQUEST_STATE_ERROR;
      return;
    }
    memcpy(new_buffer, request->receive_buffer, request->receive_buffer_used);
    request->receive_buffer = new_buffer;
    request->receive_buffer_size = new_size;
    /* Old buffer stays in arena, freed on request destroy */
  }

  memcpy(request->receive_buffer + request->receive_buffer_used, data, len);
  request->receive_buffer_used += len;

  printf("[DEBUG] Total buffered: %zu bytes, parsing...\n", request->receive_buffer_used);

  /* Print first 200 bytes of buffer for debugging */
  printf("[DEBUG] Buffer content (first 200 bytes):\n");
  size_t print_len = request->receive_buffer_used < 200 ? request->receive_buffer_used : 200;
  for (size_t i = 0; i < print_len; i++) {
    char c = request->receive_buffer[i];
    if (c >= 32 && c <= 126) {
      putchar(c);
    } else if (c == '\r') {
      printf("\\r");
    } else if (c == '\n') {
      printf("\\n\n");
    } else {
      printf("\\x%02x", (unsigned char)c);
    }
  }
  printf("\n[DEBUG] End of buffer dump\n");
  fflush(stdout);

  printf("[DEBUG] Parser state: data=%p\n", request->parser.data);
  printf("[DEBUG] Parser type: %d\n", request->parser.type);
  printf("[DEBUG] Buffer ptr: %p, size: %zu\n", (void*)request->receive_buffer, request->receive_buffer_used);

  /* Verify pointers before parsing */
  if (!request->receive_buffer || request->receive_buffer_used == 0) {
    printf("[DEBUG] ERROR: Invalid buffer!\n");
    fflush(stdout);
    return;
  }

  if (!request->parser.data) {
    printf("[DEBUG] ERROR: Parser data is NULL!\n");
    fflush(stdout);
    return;
  }

  printf("[DEBUG] About to call llhttp_execute\n");
  fflush(stdout);

  /* Parse with llhttp - using a timeout mechanism */
  printf("[DEBUG] Calling llhttp_execute NOW...\n");
  printf("[DEBUG] Attempting to parse %zu bytes at %p\n", request->receive_buffer_used, (void*)request->receive_buffer);
  fflush(stdout);

  /* Try calling with smaller chunk first to test */
  size_t test_size = request->receive_buffer_used < 50 ? request->receive_buffer_used : 50;
  printf("[DEBUG] Test parse first 50 bytes...\n");
  fflush(stdout);

  enum llhttp_errno err =
      llhttp_execute(&request->parser, request->receive_buffer, test_size);

  printf("[DEBUG] Back from llhttp_execute (test): err=%d\n", err);
  fflush(stdout);

  if (err == HPE_OK && test_size < request->receive_buffer_used) {
    printf("[DEBUG] Test succeeded, parsing remaining bytes...\n");
    fflush(stdout);
    err = llhttp_execute(&request->parser, request->receive_buffer + test_size,
                         request->receive_buffer_used - test_size);
    printf("[DEBUG] Back from second llhttp_execute\n");
    fflush(stdout);
  }

  printf("[DEBUG] llhttp_execute returned: %s (%d)\n", llhttp_errno_name(err), err);
  fflush(stdout);

  if (err != HPE_OK && err != HPE_PAUSED_UPGRADE && err != HPE_PAUSED) {
    printf("[DEBUG] Parse error detected\n");
    /* Phase HTTP-1: Allocate error from arena */
    request->response->error = (char *)turbo_arena_alloc(&request->request_arena, 256);
    if (request->response->error) {
      stbsp_snprintf(request->response->error, 256, "Parse error: %s", llhttp_errno_name(err));
    }
    request->response->error_code = HTTP_ASYNC_ERROR_PARSE_FAILED;
    request->state = REQUEST_STATE_ERROR;
    complete_request(request);
    return;
  }

  /* Extract headers if complete */
  if (request->parser_ctx.headers_complete && !request->response->headers) {
    const char *headers_end = strstr(request->receive_buffer, "\r\n\r\n");
    if (headers_end) {
      size_t headers_len = headers_end - request->receive_buffer;
      /* Phase HTTP-1: Allocate headers from arena */
      request->response->headers = (char *)turbo_arena_alloc(&request->request_arena, headers_len + 1);
      if (request->response->headers) {
        memcpy(request->response->headers, request->receive_buffer, headers_len);
        request->response->headers[headers_len] = '\0';
        request->response->headers_len = headers_len;
      }
    }
  }

  /* Call progress callback if set */
  if (request->progress_callback) {
    size_t total = 0;
    char *content_length_str = get_header_value(request->response->headers, "Content-Length");
    if (content_length_str) {
      total = (size_t)atoll(content_length_str);
      free(content_length_str);
    }
    request->progress_callback(request, request->response->body_len, total,
                               request->progress_user_data);
  }

  /* Check if message is complete (headers_complete == 2 means on_message_complete was called) */
  if (request->parser_ctx.headers_complete == 2) {
    printf("[DEBUG] Message complete, calling complete_request\n");
    fflush(stdout);
    complete_request(request);
  }
}

/* Helper functions implementation */

static const char *method_to_string(http_method_t method) {
  return llhttp_method_name((enum llhttp_method)method);
}

/* uri_parser is used for URL parsing */


static char *get_header_value(const char *headers, const char *header_name) {
  if (!headers || !header_name)
    return NULL;

  size_t name_len = strlen(header_name);
  const char *p = headers;

  while (*p) {
    if (strncasecmp(p, header_name, name_len) == 0 && p[name_len] == ':') {
      p += name_len + 1;
      while (*p == ' ' || *p == '\t')
        p++;

      const char *end = strstr(p, "\r\n");
      if (!end)
        end = p + strlen(p);

      size_t value_len = end - p;
      char *value = malloc(value_len + 1);
      memcpy(value, p, value_len);
      value[value_len] = '\0';
      return value;
    }

    p = strstr(p, "\r\n");
    if (!p)
      break;
    p += 2;
  }

  return NULL;
}

static char *build_full_url(http_async_client_t *client, const char *url, turbo_arena_t *arena) {
  if (!client->base_url || strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
    return NULL;
  }

  size_t base_len = strlen(client->base_url);
  size_t url_len = strlen(url);
  size_t full_len = base_len + url_len + 2;

  /* Phase HTTP-1: Allocate from arena instead of malloc */
  char *full_url = (char *)turbo_arena_alloc(arena, full_len);
  if (!full_url)
    return NULL;

  if (url[0] == '/') {
    stbsp_snprintf(full_url, (int)full_len, "%s%s", client->base_url, url);
  } else {
    stbsp_snprintf(full_url, (int)full_len, "%s/%s", client->base_url, url);
  }

  return full_url;
}

static void initiate_request(http_async_request_t *request) {
  if (!request)
    return;

  printf("[DEBUG] initiate_request called\n");

  /* Parse URL */
  if (turbo_parse_uri((const uint8_t *)request->url, strlen(request->url), &request->uri) != 0) {
    printf("[DEBUG] URL parse failed\n");
    /* Phase HTTP-1: Allocate error from arena */
    request->response->error = turbo_arena_strdup(&request->request_arena, "Failed to parse URL");
    request->response->error_code = HTTP_ASYNC_ERROR_INVALID_URL;
    request->state = REQUEST_STATE_ERROR;
    complete_request(request);
    return;
  }

  const char *uri_scheme = turbo_uri_scheme(request->uri);
  const char *uri_host = turbo_uri_host(request->uri);
  int uri_port = turbo_uri_port(request->uri);

  int is_tls = (strcasecmp(uri_scheme, "https") == 0);
  printf("[DEBUG] Parsed URL - host: %s, port: %d, scheme: %s, TLS: %d\n",
         uri_host, uri_port, uri_scheme, is_tls);


  /* Initialize llhttp parser - use request->parser_settings to keep it alive! */
  printf("[DEBUG] Initializing llhttp settings\n");
  fflush(stdout);
  llhttp_settings_init(&request->parser_settings);
  printf("[DEBUG] Setting callbacks\n");
  fflush(stdout);
  request->parser_settings.on_status = on_status;
  request->parser_settings.on_header_field = on_header_field;
  request->parser_settings.on_header_value = on_header_value;
  request->parser_settings.on_headers_complete = on_headers_complete;
  request->parser_settings.on_body = on_body;
  request->parser_settings.on_message_complete = on_message_complete;

  printf("[DEBUG] Initializing parser with HTTP_RESPONSE type\n");
  fflush(stdout);
  llhttp_init(&request->parser, HTTP_RESPONSE, &request->parser_settings);
  printf("[DEBUG] Setting parser.data = %p\n", (void*)&request->parser_ctx);
  fflush(stdout);
  request->parser.data = &request->parser_ctx;
  request->parser_ctx.response = request->response;
  printf("[DEBUG] Parser initialization complete\n");
  fflush(stdout);

  /* Initiate connection */
  request->state = REQUEST_STATE_CONNECTING;

  printf("[DEBUG] Calling async_client_connect(%s)\n", request->url);

  async_client_status_t status = async_client_connect(request->client->client, request->url);


  printf("[DEBUG] async_client_connect returned: %d\n", status);

  if (status != ASYNC_CLIENT_STATUS_OK) {
    printf("[DEBUG] Connection failed with status %d\n", status);
    /* Phase HTTP-1: Allocate error from arena */
    request->response->error = turbo_arena_strdup(&request->request_arena, "Failed to initiate connection");
    request->response->error_code = HTTP_ASYNC_ERROR_CONNECTION_FAILED;
    request->state = REQUEST_STATE_ERROR;
    complete_request(request);
  } else {
    printf("[DEBUG] async_client_connect succeeded, waiting for CONNECTED event\n");
  }
}

static void send_http_request(http_async_request_t *request) {
  if (!request)
    return;

  request->state = REQUEST_STATE_SENDING;

  /* Build HTTP request */
  char request_line[1024];
  const char *uri_path = turbo_uri_path(request->uri);
  const char *uri_query = turbo_uri_query(request->uri);
  const char *uri_host = turbo_uri_host(request->uri);

  stbsp_snprintf(request_line, sizeof(request_line), "%s %s%s%s HTTP/1.1\r\n",
           method_to_string(request->method), 
           uri_path[0] ? uri_path : "/",
           uri_query[0] ? "?" : "", 
           uri_query);

  char host_header[512];
  stbsp_snprintf(host_header, sizeof(host_header), "Host: %s\r\n", uri_host);

  char ua_header[256];
  stbsp_snprintf(ua_header, sizeof(ua_header), "User-Agent: %s\r\n", request->client->user_agent);

  char content_length[128] = "";
  if (request->body && request->body_len > 0) {
    stbsp_snprintf(content_length, sizeof(content_length), "Content-Length: %zu\r\n", request->body_len);
  }

  /* Use scatter-gather send */
  async_client_iovec_t iov[64];
  int iov_count = 0;

  iov[iov_count].data = (char*)request_line;
  iov[iov_count].len = strlen(request_line);
  iov_count++;

  iov[iov_count].data = (char*)host_header;
  iov[iov_count].len = strlen(host_header);
  iov_count++;

  iov[iov_count].data = (char*)ua_header;
  iov[iov_count].len = strlen(ua_header);
  iov_count++;

  if (content_length[0]) {
    iov[iov_count].data = (char*)content_length;
    iov[iov_count].len = strlen(content_length);
    iov_count++;
  }

  /* Add auth header if set */
  if (request->client->auth_header) {
    iov[iov_count].data = (char*)request->client->auth_header;
    iov[iov_count].len = strlen(request->client->auth_header);
    iov_count++;

    iov[iov_count].data = (char*)"\r\n";
    iov[iov_count].len = 2;
    iov_count++;
  }

  /* Add default headers */
  char **default_header_strings = NULL;
  int default_headers_added = 0;
  if (request->client->default_headers) {
    default_header_strings = malloc(sizeof(char *) * request->client->default_header_count);
    if (default_header_strings) {
      header_entry_t *entry = request->client->default_headers;
      while (entry && iov_count < 58) {
        size_t len = strlen(entry->name) + strlen(entry->value) + 5;
        char *header_str = malloc(len);
        if (header_str) {
          stbsp_snprintf(header_str, (int)len, "%s: %s\r\n", entry->name, entry->value);
          default_header_strings[default_headers_added] = header_str;

          iov[iov_count].data = (char*)header_str;
          iov[iov_count].len = strlen(header_str);
          iov_count++;
          default_headers_added++;
        }
        entry = entry->next;
      }
    }
  }

  /* Add custom headers */
  for (int i = 0; i < request->header_count && iov_count < 60; i++) {
    iov[iov_count].data = (char*)request->headers[i];
    iov[iov_count].len = strlen(request->headers[i]);
    iov_count++;

    iov[iov_count].data = (char*)"\r\n";
    iov[iov_count].len = 2;
    iov_count++;
  }

  /* End of headers */
  iov[iov_count].data = (char*)"\r\n";
  iov[iov_count].len = 2;
  iov_count++;

  /* Add body if present */
  if (request->body && request->body_len > 0) {
    iov[iov_count].data = (char*)request->body;
    iov[iov_count].len = request->body_len;
    iov_count++;
  }

  /* Debug: Print the complete HTTP request */
  printf("[DEBUG] === HTTP Request (iov_count=%d) ===\n", iov_count);
  for (int i = 0; i < iov_count; i++) {
    printf("%.*s", (int)iov[i].len, iov[i].data);
  }
  printf("\n[DEBUG] === End HTTP Request ===\n");
  fflush(stdout);

  /* Send request */
  printf("[DEBUG] Calling async_client_sendv...\n");
  fflush(stdout);
  async_client_status_t status = async_client_sendv(request->client->client, iov, iov_count);
  printf("[DEBUG] async_client_sendv returned: %d\n", status);
  fflush(stdout);

  /* Free default header strings */
  if (default_header_strings) {
    for (int i = 0; i < default_headers_added; i++) {
      free(default_header_strings[i]);
    }
    free(default_header_strings);
  }

  if (status == ASYNC_CLIENT_STATUS_OK) {
    request->state = REQUEST_STATE_RECEIVING;
    request->client->stats.bytes_sent += request->body_len;
  } else {
    /* Phase HTTP-1: Allocate error from arena */
    request->response->error = turbo_arena_strdup(&request->request_arena, "Failed to send request");
    request->response->error_code = HTTP_ASYNC_ERROR_SEND_FAILED;
    request->state = REQUEST_STATE_ERROR;
    complete_request(request);
  }
}

static void complete_request(http_async_request_t *request) {
  if (!request)
    return;

  printf("[DEBUG] complete_request called, current state: %d\n", request->state);

  if (request->state == REQUEST_STATE_CANCELLED) {
    /* Phase HTTP-1: Allocate error from arena */
    request->response->error = turbo_arena_strdup(&request->request_arena, "Request cancelled");
    request->response->error_code = HTTP_ASYNC_ERROR_CANCELLED;
  }

  request->state = REQUEST_STATE_COMPLETE;
  printf("[DEBUG] Request state set to COMPLETE\n");

  /* Update stats */
  if (!request->response->error) {
    request->client->stats.successful_requests++;
    request->client->stats.bytes_received += request->response->body_len;
  } else {
    request->client->stats.failed_requests++;
  }

  /* Check for redirects */
  if (request->client->follow_redirects &&
      request->redirect_count < request->client->max_redirects &&
      (request->response->status_code == 301 || request->response->status_code == 302 ||
       request->response->status_code == 303 || request->response->status_code == 307 ||
       request->response->status_code == 308)) {

    char *location = get_header_value(request->response->headers, "Location");
    if (location) {
      /* TODO: Handle redirect - would need to create new request */
      free(location);
    }
  }

  /* Invoke callback */
  printf("[DEBUG] About to invoke callback (callback=%p)\n", (void*)request->callback);
  if (request->callback) {
    printf("[DEBUG] Calling callback...\n");
    request->callback(request, request->response, request->user_data);
    printf("[DEBUG] Callback returned\n");
  } else {
    printf("[DEBUG] WARNING: No callback set!\n");
  }

  /* Cleanup */
  free(request->parser_ctx.current_header_field);
  free(request->parser_ctx.current_header_value);

  printf("[DEBUG] Removing request from active list\n");
  remove_request(request->client, request);
  printf("[DEBUG] complete_request finished\n");
}

static void remove_request(http_async_client_t *client, http_async_request_t *request) {
  if (!client || !request)
    return;

  /* Remove from linked list */
  if (request->prev) {
    request->prev->next = request->next;
  } else {
    client->active_requests = request->next;
  }

  if (request->next) {
    request->next->prev = request->prev;
  }

  client->stats.active_requests--;

  /* Phase HTTP-1 Optimization: Single arena_free() replaces 10+ individual free() calls!
   * This frees: url, body, receive_buffer, headers array, all header strings, response,
   * response->headers, response->error
   * Before: free(url) + free(body) + free(receive_buffer) + free_parsed_url(4 frees) +
   *         loop free(headers[i]) + free(headers) = 10+ free() calls
   * After: ONE call frees everything! */
  if (request->uri) {
    turbo_free_uri(&request->uri);
  }
  turbo_arena_free(&request->request_arena);

  /* Note: response is not freed here - caller owns it after callback
   * But response data (headers, error) was allocated from arena and already freed above */
  free(request);
}

/* ============================================================================
 * Additional Response Helpers
 * ========================================================================= */

int http_async_response_has_header(http_async_response_t *response, const char *name) {
  char *value = http_async_response_get_header(response, name);
  int has = (value != NULL);
  free(value);
  return has;
}

char *http_async_response_content_type(http_async_response_t *response) {
  return http_async_response_get_header(response, "Content-Type");
}

size_t http_async_response_content_length(http_async_response_t *response) {
  char *value = http_async_response_get_header(response, "Content-Length");
  if (!value)
    return 0;

  size_t length = (size_t)atoll(value);
  free(value);
  return length;
}

int http_async_response_is_html(http_async_response_t *response) {
  char *content_type = http_async_response_content_type(response);
  if (!content_type)
    return 0;

  int is_html = (strstr(content_type, "text/html") != NULL);
  free(content_type);
  return is_html;
}

int http_async_response_is_text(http_async_response_t *response) {
  char *content_type = http_async_response_content_type(response);
  if (!content_type)
    return 0;

  int is_text = (strstr(content_type, "text/") != NULL);
  free(content_type);
  return is_text;
}

/* ============================================================================
 * URL Parameters / Form Data
 * ========================================================================= */

http_async_params_t *http_async_params_create(void) {
  http_async_params_t *params = calloc(1, sizeof(http_async_params_t));
  return params;
}

void http_async_params_add(http_async_params_t *params, const char *key, const char *value) {
  if (!params || !key || !value)
    return;

  struct param_entry *entry = malloc(sizeof(struct param_entry));
  if (!entry)
    return;

  entry->key = strdup(key);
  entry->value = strdup(value);
  entry->next = params->head;
  params->head = entry;
  params->count++;
}

static char *url_encode(const char *str) {
  if (!str)
    return NULL;

  size_t len = strlen(str);
  char *encoded = malloc(len * 3 + 1);
  if (!encoded)
    return NULL;

  char *p = encoded;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];

    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
      *p++ = c;
    } else if (c == ' ') {
      *p++ = '+';
    } else {
      sprintf(p, "%%%02X", c);
      p += 3;
    }
  }
  *p = '\0';

  return encoded;
}

char *http_async_params_encode(http_async_params_t *params) {
  if (!params || !params->head)
    return strdup("");

  size_t size = 0;
  struct param_entry *entry = params->head;
  while (entry) {
    char *key_enc = url_encode(entry->key);
    char *val_enc = url_encode(entry->value);

    if (key_enc && val_enc) {
      size += strlen(key_enc) + strlen(val_enc) + 2;
    }

    free(key_enc);
    free(val_enc);
    entry = entry->next;
  }

  if (size == 0)
    return strdup("");

  char *result = malloc(size + 1);
  if (!result)
    return NULL;

  char *p = result;
  entry = params->head;
  int first = 1;

  while (entry) {
    char *key_enc = url_encode(entry->key);
    char *val_enc = url_encode(entry->value);

    if (key_enc && val_enc) {
      if (!first) {
        *p++ = '&';
      }
      strcpy(p, key_enc);
      p += strlen(key_enc);
      *p++ = '=';
      strcpy(p, val_enc);
      p += strlen(val_enc);
      first = 0;
    }

    free(key_enc);
    free(val_enc);
    entry = entry->next;
  }
  *p = '\0';

  return result;
}

void http_async_params_free(http_async_params_t *params) {
  if (!params)
    return;

  struct param_entry *entry = params->head;
  while (entry) {
    struct param_entry *next = entry->next;
    free(entry->key);
    free(entry->value);
    free(entry);
    entry = next;
  }

  free(params);
}

char *http_async_build_url(const char *base_url, http_async_params_t *query_params) {
  if (!base_url)
    return NULL;

  if (!query_params || !query_params->head)
    return strdup(base_url);

  char *query_string = http_async_params_encode(query_params);
  if (!query_string)
    return strdup(base_url);

  const char *has_query = strchr(base_url, '?');
  char separator = has_query ? '&' : '?';

  size_t url_len = strlen(base_url) + strlen(query_string) + 2;
  char *full_url = malloc(url_len);
  if (!full_url) {
    free(query_string);
    return NULL;
  }

  stbsp_snprintf(full_url, (int)url_len, "%s%c%s", base_url, separator, query_string);
  free(query_string);

  return full_url;
}

http_async_request_t *http_async_post_form(http_async_client_t *client, const char *url,
                                           http_async_params_t *params,
                                           http_async_response_cb callback, void *user_data) {
  if (!client || !url || !params)
    return NULL;

  char *form_data = http_async_params_encode(params);
  if (!form_data)
    return NULL;

  const char *headers[] = {"Content-Type: application/x-www-form-urlencoded"};

  http_async_request_t *request = http_async_request(client, HTTP_POST, url, headers, 1, form_data,
                                                     strlen(form_data), callback, user_data);

  free(form_data);
  return request;
}

/* ============================================================================
 * Cookie Management
 * ========================================================================= */

http_async_cookie_jar_t *http_async_cookie_jar_create(void) {
  http_async_cookie_jar_t *jar = calloc(1, sizeof(http_async_cookie_jar_t));
  return jar;
}

void http_async_cookie_jar_destroy(http_async_cookie_jar_t *jar) {
  if (!jar)
    return;

  http_async_cookie_t *cookie = jar->cookies;
  while (cookie) {
    http_async_cookie_t *next = cookie->next;
    free(cookie->name);
    free(cookie->value);
    free(cookie->domain);
    free(cookie->path);
    free(cookie);
    cookie = next;
  }

  free(jar);
}

void http_async_cookie_jar_set(http_async_cookie_jar_t *jar, const char *name, const char *value) {
  if (!jar || !name || !value)
    return;

  http_async_cookie_t *cookie = jar->cookies;
  while (cookie) {
    if (strcmp(cookie->name, name) == 0) {
      free(cookie->value);
      cookie->value = strdup(value);
      return;
    }
    cookie = cookie->next;
  }

  cookie = calloc(1, sizeof(http_async_cookie_t));
  if (!cookie)
    return;

  cookie->name = strdup(name);
  cookie->value = strdup(value);
  cookie->next = jar->cookies;
  jar->cookies = cookie;
  jar->count++;
}

const char *http_async_cookie_jar_get(http_async_cookie_jar_t *jar, const char *name) {
  if (!jar || !name)
    return NULL;

  http_async_cookie_t *cookie = jar->cookies;
  while (cookie) {
    if (strcmp(cookie->name, name) == 0) {
      return cookie->value;
    }
    cookie = cookie->next;
  }

  return NULL;
}

void http_async_cookie_jar_remove(http_async_cookie_jar_t *jar, const char *name) {
  if (!jar || !name)
    return;

  http_async_cookie_t **prev = &jar->cookies;
  http_async_cookie_t *cookie = jar->cookies;

  while (cookie) {
    if (strcmp(cookie->name, name) == 0) {
      *prev = cookie->next;
      free(cookie->name);
      free(cookie->value);
      free(cookie->domain);
      free(cookie->path);
      free(cookie);
      jar->count--;
      return;
    }
    prev = &cookie->next;
    cookie = cookie->next;
  }
}

void http_async_cookie_jar_clear(http_async_cookie_jar_t *jar) {
  if (!jar)
    return;

  http_async_cookie_t *cookie = jar->cookies;
  while (cookie) {
    http_async_cookie_t *next = cookie->next;
    free(cookie->name);
    free(cookie->value);
    free(cookie->domain);
    free(cookie->path);
    free(cookie);
    cookie = next;
  }

  jar->cookies = NULL;
  jar->count = 0;
}

int http_async_cookie_jar_count(http_async_cookie_jar_t *jar) { return jar ? jar->count : 0; }

void http_async_client_set_cookie_jar(http_async_client_t *client, http_async_cookie_jar_t *jar) {
  if (client) {
    client->cookie_jar = jar;
  }
}

http_async_cookie_jar_t *http_async_client_get_cookie_jar(http_async_client_t *client) {
  return client ? client->cookie_jar : NULL;
}

/* ============================================================================
 * Multipart Form Data
 * ========================================================================= */

static void generate_boundary(char *boundary, size_t len) {
  const char *chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  size_t chars_len = strlen(chars);

  strcpy(boundary, "----WebKitFormBoundary");
  size_t prefix_len = strlen(boundary);

  for (size_t i = prefix_len; i < len - 1; i++) {
    boundary[i] = chars[rand() % chars_len];
  }
  boundary[len - 1] = '\0';
}

http_async_multipart_form_t *http_async_multipart_form_create(void) {
  http_async_multipart_form_t *form = calloc(1, sizeof(http_async_multipart_form_t));
  if (!form)
    return NULL;

  generate_boundary(form->boundary, sizeof(form->boundary));

  return form;
}

void http_async_multipart_form_destroy(http_async_multipart_form_t *form) {
  if (!form)
    return;

  http_async_multipart_part_t *part = form->parts;
  while (part) {
    http_async_multipart_part_t *next = part->next;
    free(part->name);
    free(part->filename);
    free(part->content_type);
    free(part->value);
    free(part->data);
    free(part);
    part = next;
  }

  free(form);
}

void http_async_multipart_form_add_field(http_async_multipart_form_t *form, const char *name,
                                         const char *value) {
  if (!form || !name || !value)
    return;

  http_async_multipart_part_t *part = calloc(1, sizeof(http_async_multipart_part_t));
  if (!part)
    return;

  part->name = strdup(name);
  part->value = strdup(value);
  part->is_file = 0;
  part->next = form->parts;
  form->parts = part;
  form->part_count++;
}

void http_async_multipart_form_add_file(http_async_multipart_form_t *form, const char *field_name,
                                        const char *filename, const char *content_type,
                                        const void *data, size_t data_len) {
  if (!form || !field_name || !filename || !data)
    return;

  http_async_multipart_part_t *part = calloc(1, sizeof(http_async_multipart_part_t));
  if (!part)
    return;

  part->name = strdup(field_name);
  part->filename = strdup(filename);
  part->content_type = content_type ? strdup(content_type) : strdup("application/octet-stream");
  part->data = malloc(data_len);
  if (part->data) {
    memcpy(part->data, data, data_len);
    part->data_len = data_len;
  }
  part->is_file = 1;
  part->next = form->parts;
  form->parts = part;
  form->part_count++;
}

int http_async_multipart_form_add_file_path(http_async_multipart_form_t *form,
                                            const char *field_name, const char *file_path,
                                            const char *content_type) {
  if (!form || !field_name || !file_path)
    return -1;

  FILE *fp = fopen(file_path, "rb");
  if (!fp)
    return -1;

  fseek(fp, 0, SEEK_END);
  long file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  if (file_size < 0 || file_size > 100 * 1024 * 1024) {
    fclose(fp);
    return -1;
  }

  void *data = malloc((size_t)file_size);
  if (!data) {
    fclose(fp);
    return -1;
  }

  size_t read_size = fread(data, 1, (size_t)file_size, fp);
  fclose(fp);

  if (read_size != (size_t)file_size) {
    free(data);
    return -1;
  }

  const char *filename = strrchr(file_path, '/');
  if (!filename)
    filename = strrchr(file_path, '\\');
  filename = filename ? filename + 1 : file_path;

  http_async_multipart_form_add_file(form, field_name, filename, content_type, data, read_size);
  free(data);

  return 0;
}

static char *build_multipart_body(http_async_multipart_form_t *form, size_t *body_len) {
  if (!form || !body_len)
    return NULL;

  size_t total_size = 0;
  http_async_multipart_part_t *part = form->parts;

  while (part) {
    total_size += strlen(form->boundary) + 4;
    total_size += 100 + strlen(part->name);
    if (part->filename) {
      total_size += strlen(part->filename) + 20;
    }
    if (part->is_file && part->content_type) {
      total_size += strlen(part->content_type) + 20;
    }
    total_size += 2;
    if (part->is_file) {
      total_size += part->data_len;
    } else {
      total_size += strlen(part->value);
    }
    total_size += 2;
    part = part->next;
  }

  total_size += strlen(form->boundary) + 6;

  char *body = malloc(total_size + 1);
  if (!body)
    return NULL;

  char *p = body;
  part = form->parts;

  while (part) {
    p += sprintf(p, "--%s\r\n", form->boundary);

    if (part->is_file) {
      p += sprintf(p, "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n",
                   part->name, part->filename);
      p += sprintf(p, "Content-Type: %s\r\n", part->content_type);
    } else {
      p += sprintf(p, "Content-Disposition: form-data; name=\"%s\"\r\n", part->name);
    }

    p += sprintf(p, "\r\n");

    if (part->is_file) {
      memcpy(p, part->data, part->data_len);
      p += part->data_len;
    } else {
      strcpy(p, part->value);
      p += strlen(part->value);
    }

    p += sprintf(p, "\r\n");

    part = part->next;
  }

  p += sprintf(p, "--%s--\r\n", form->boundary);

  *body_len = p - body;
  return body;
}

http_async_request_t *http_async_post_multipart(http_async_client_t *client, const char *url,
                                                http_async_multipart_form_t *form,
                                                http_async_response_cb callback, void *user_data) {
  if (!client || !url || !form)
    return NULL;

  size_t body_len;
  char *body = build_multipart_body(form, &body_len);
  if (!body)
    return NULL;

  char content_type[256];
  stbsp_snprintf(content_type, sizeof(content_type), "Content-Type: multipart/form-data; boundary=%s",
           form->boundary);

  const char *headers[] = {content_type};

  http_async_request_t *request =
      http_async_request(client, HTTP_POST, url, headers, 1, body, body_len, callback, user_data);

  free(body);
  return request;
}

/* ============================================================================
 * Interceptors
 * ========================================================================= */

void http_async_client_add_request_interceptor(http_async_client_t *client,
                                               http_async_request_interceptor_t interceptor,
                                               void *user_data) {
  if (!client || !interceptor)
    return;

  http_async_interceptor_node_t *node = malloc(sizeof(http_async_interceptor_node_t));
  if (!node)
    return;

  node->callback.request = interceptor;
  node->user_data = user_data;
  node->next = client->request_interceptors;
  client->request_interceptors = node;
}

void http_async_client_add_response_interceptor(http_async_client_t *client,
                                                http_async_response_interceptor_t interceptor,
                                                void *user_data) {
  if (!client || !interceptor)
    return;

  http_async_interceptor_node_t *node = malloc(sizeof(http_async_interceptor_node_t));
  if (!node)
    return;

  node->callback.response = interceptor;
  node->user_data = user_data;
  node->next = client->response_interceptors;
  client->response_interceptors = node;
}

void http_async_client_clear_interceptors(http_async_client_t *client) {
  if (!client)
    return;

  http_async_interceptor_node_t *node = client->request_interceptors;
  while (node) {
    http_async_interceptor_node_t *next = node->next;
    free(node);
    node = next;
  }
  client->request_interceptors = NULL;

  node = client->response_interceptors;
  while (node) {
    http_async_interceptor_node_t *next = node->next;
    free(node);
    node = next;
  }
  client->response_interceptors = NULL;
}

/* ============================================================================
 * Retry Policy
 * ========================================================================= */

http_async_retry_policy_t http_async_retry_policy_default(void) {
  http_async_retry_policy_t policy = {.max_retries = 3,
                                      .initial_delay_ms = 1000,
                                      .max_delay_ms = 30000,
                                      .exponential_backoff = 1,
                                      .retry_on_timeout = 1,
                                      .retry_on_connection_error = 1,
                                      .retry_on_5xx = 1,
                                      .jitter_factor = 0.1};
  return policy;
}

void http_async_client_set_retry_policy(http_async_client_t *client,
                                        const http_async_retry_policy_t *policy) {
  if (!client || !policy)
    return;

  client->retry_policy = *policy;
  client->has_retry_policy = 1;
}

void http_async_client_get_retry_policy(http_async_client_t *client,
                                        http_async_retry_policy_t *policy) {
  if (!client || !policy)
    return;

  if (client->has_retry_policy) {
    *policy = client->retry_policy;
  } else {
    memset(policy, 0, sizeof(http_async_retry_policy_t));
  }
}

void http_async_client_clear_retry_policy(http_async_client_t *client) {
  if (!client)
    return;

  client->has_retry_policy = 0;
  memset(&client->retry_policy, 0, sizeof(http_async_retry_policy_t));
}

/* ============================================================================
 * Compression Support
 * ========================================================================= */

void http_async_client_enable_compression(http_async_client_t *client, int enable) {
  if (client) {
    client->compression_enabled = enable;
  }
}

int http_async_client_is_compression_enabled(http_async_client_t *client) {
  return client ? client->compression_enabled : 0;
}

/* ============================================================================
 * Rate Limiting
 * ========================================================================= */

#ifdef _WIN32
  #include <sys/timeb.h>
static double get_time_seconds(void) {
  struct _timeb tb;
  _ftime(&tb);
  return (double)tb.time + (double)tb.millitm / 1000.0;
}
#else
  #include <sys/time.h>
static double get_time_seconds(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}
#endif

void http_async_client_set_rate_limit(http_async_client_t *client,
                                      const http_async_rate_limit_t *limit) {
  if (!client || !limit)
    return;

  client->rate_limit = *limit;
  if (client->rate_limit.burst_size == 0) {
    client->rate_limit.burst_size = client->rate_limit.requests_per_second;
  }
  client->has_rate_limit = 1;
  client->tokens = client->rate_limit.burst_size;
  client->last_request_time = get_time_seconds();
}

void http_async_client_clear_rate_limit(http_async_client_t *client) {
  if (!client)
    return;

  client->has_rate_limit = 0;
}

int http_async_client_has_rate_limit(http_async_client_t *client) {
  return client ? client->has_rate_limit : 0;
}

/* ============================================================================
 * JSON Helpers
 * ========================================================================= */

json_value_t *http_async_response_parse_json(http_async_response_t *response) {
  if (!response || !response->body || response->body_len == 0)
    return NULL;

  json_value_t *val = NULL;
  if (turbo_parse_json((const uint8_t *)response->body, response->body_len, &val) == 0) {
    return val;
  }
  return NULL;
}

http_async_request_t *http_async_post_json(http_async_client_t *client, const char *url,
                                           const char *json_string, http_async_response_cb callback,
                                           void *user_data) {
  if (!client || !url || !json_string)
    return NULL;

  const char *headers[] = {"Content-Type: application/json"};

  return http_async_request(client, HTTP_POST, url, headers, 1, json_string, strlen(json_string),
                            callback, user_data);
}

http_async_request_t *http_async_post_json_object(http_async_client_t *client, const char *url,
                                                   json_value_t *json_obj, http_async_response_cb callback,
                                                   void *user_data) {
  if (!client || !url || !json_obj)
    return NULL;

  char *json_string = turbo_json_serialize(json_obj, NULL);
  if (!json_string)
    return NULL;

  http_async_request_t *request =
      http_async_post_json(client, url, json_string, callback, user_data);
  turbo_json_serialize_free(json_string);

  return request;
}

int http_async_response_decode_jwt(http_async_response_t *response, const uint8_t *key, size_t key_len, uint32_t options, void **jwt) {
  if (!response || !response->body || !jwt)
    return CJWTE_INVALID_PARAMETERS;

  int64_t current_time = (int64_t)time(NULL);
  return (int)cjwt_decode(response->body, response->body_len, options, key, key_len, current_time, 0, (cjwt_t **)jwt);
}

void http_async_jwt_destroy(void *jwt) {
  if (jwt) {
    cjwt_destroy((cjwt_t *)jwt);
  }
}
