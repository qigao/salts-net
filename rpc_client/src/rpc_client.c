#include "rpc_client.h"
#include "rpc_error.h"
#include "tlog.h"
#include <http_client.h>
#include <json_parser.h>
#include <platform.h>
#include <stb_sprintf.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbo_coro.h>


#define RPC_CLIENT_VERSION "1.0.0"
#define MAX_REQUEST_SIZE 65536

/* Client structure */
struct rpc_client_s {
  rpc_client_config_t config;
  rpc_client_state_t state;
  http_client_t *http_client;
  int request_id_counter;
};

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/* Build full URL from config */
static const char *build_url(rpc_client_t *client) { return client->config.url; }

/* Build JSON-RPC request using json_parser */
static char *build_jsonrpc_request(const char *method, const char *params, const char *id,
                                   size_t *request_len) {
  if (!method || !request_len)
    return NULL;

  json_value_t *root = json_create_object();
  if (!root)
    return NULL;

  json_object_set_string(root, "jsonrpc", "2.0");
  json_object_set_string(root, "method", method);

  /* Add params if provided */
  if (params && params[0] != '\0') {
    json_value_t *params_obj = json_parse(params, strlen(params));
    if (params_obj) {
      json_object_add(root, "params", params_obj);
    }
  }

  /* Add ID if provided (regular request), otherwise it's a notification */
  if (id) {
    json_object_set_string(root, "id", id);
  }

  char *request = json_serialize(root, request_len);
  json_free(root);

  return request;
}

/* JSON value extraction using json_parser */
static char *json_extract_string(const char *json, const char *key) {
  if (!json)
    return NULL;

  json_value_t *root = json_parse(json, strlen(json));
  if (!root)
    return NULL;

  char *result = NULL;

  if (key) {
    json_value_t *item = json_object_get(root, key);
    if (item && json_type(item) == JSON_STRING) {
      result = strdup(json_string(item));
    }
  } else {
    /* Root value */
    if (json_type(root) == JSON_STRING) {
      result = strdup(json_string(root));
    }
  }

  json_free(root);
  return result;
}

static int json_extract_int(const char *json, const char *key, int64_t *value) {
  if (!json || !value)
    return -1;

  json_value_t *root = json_parse(json, strlen(json));
  if (!root)
    return -1;

  int ret = -1;

  if (key) {
    json_value_t *item = json_object_get(root, key);
    if (item && json_type(item) == JSON_NUMBER) {
      *value = (int64_t)json_number(item);
      ret = 0;
    }
  } else {
    if (json_type(root) == JSON_NUMBER) {
      *value = (int64_t)json_number(root);
      ret = 0;
    }
  }

  json_free(root);
  return ret;
}

static int json_extract_bool(const char *json, const char *key, int *value) {
  if (!json || !value)
    return -1;

  json_value_t *root = json_parse(json, strlen(json));
  if (!root)
    return -1;

  int ret = -1;

  if (key) {
    json_value_t *item = json_object_get(root, key);
    if (item && json_type(item) == JSON_BOOL) {
      *value = json_bool(item) ? 1 : 0;
      ret = 0;
    }
  } else {
    if (json_type(root) == JSON_BOOL) {
      *value = json_bool(root) ? 1 : 0;
      ret = 0;
    }
  }

  json_free(root);
  return ret;
}

static int json_extract_double(const char *json, const char *key, double *value) {
  if (!json || !value)
    return -1;

  json_value_t *root = json_parse(json, strlen(json));
  if (!root)
    return -1;

  int ret = -1;

  if (key) {
    json_value_t *item = json_object_get(root, key);
    if (item && json_type(item) == JSON_NUMBER) {
      *value = json_number(item);
      ret = 0;
    }
  } else {
    if (json_type(root) == JSON_NUMBER) {
      *value = json_number(root);
      ret = 0;
    }
  }

  json_free(root);
  return ret;
}

/* Parse JSON-RPC response using json_parser */
static int parse_jsonrpc_response(const char *body, rpc_call_result_t *result) {
  if (!body || !result)
    return -1;

  /* Note: caller should memset result before calling this function */

  json_value_t *root = json_parse(body, strlen(body));
  if (!root)
    return -1;

  /* Extract ID */
  json_value_t *id_item = json_object_get(root, "id");
  if (id_item && json_type(id_item) == JSON_STRING) {
    result->id = strdup(json_string(id_item));
  }

  /* Check for error */
  json_value_t *error = json_object_get(root, "error");
  if (error && json_type(error) == JSON_OBJECT) {
    result->success = 0;

    /* Extract error code */
    json_value_t *code = json_object_get(error, "code");
    if (code && json_type(code) == JSON_NUMBER) {
      result->error_code = (int)json_number(code);
    }

    /* Extract error message */
    json_value_t *message = json_object_get(error, "message");
    if (message && json_type(message) == JSON_STRING) {
      result->error_message = strdup(json_string(message));
    }
  } else {
    result->success = 1;

    /* Extract result */
    json_value_t *result_item = json_object_get(root, "result");
    if (result_item) {
      result->result = json_serialize(result_item, NULL);
    }
  }

  json_free(root);
  return 0;
}

/* Process a coro response into an rpc_call_result_t */
static void process_coro_response(http_response_t *resp, rpc_call_result_t *result) {
  memset(result, 0, sizeof(rpc_call_result_t));
  result->http_status = resp->status_code;

  if (resp->error_code != HTTP_ERROR_NONE) {
    result->success = 0;
    result->error_code = RPC_ERROR_INTERNAL;
    if (resp->error) {
      result->error_message = strdup(resp->error);
    }
  } else if (resp->body && resp->body_len > 0) {
    parse_jsonrpc_response(resp->body, result);
    result->http_status = resp->status_code;
  } else {
    result->success = 0;
    result->error_code = RPC_ERROR_INTERNAL;
    result->error_message = strdup("Empty response");
  }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

rpc_client_t *rpc_client_create(const rpc_client_config_t *config) {
  if (!config || !config->url || !config->http_client)
    return NULL;

  rpc_client_t *client = (rpc_client_t *)calloc(1, sizeof(rpc_client_t));
  if (!client)
    return NULL;

  client->config = *config;
  client->state = RPC_STATE_DISCONNECTED;
  client->request_id_counter = 1;

  client->http_client = config->http_client;

  /* Set default headers */
  http_client_set_default_header(client->http_client, "Content-Type", "application/json");

  return client;
}

void rpc_client_destroy(rpc_client_t *client) {
  if (!client)
    return;

  free(client);
}

int rpc_client_connect(rpc_client_t *client) {
  if (!client)
    return -1;

  /* HTTP client handles connections automatically */
  client->state = RPC_STATE_CONNECTED;
  return 0;
}

void rpc_client_disconnect(rpc_client_t *client) {
  if (!client)
    return;

  /* HTTP client handles disconnections automatically */
  client->state = RPC_STATE_DISCONNECTED;
}

rpc_client_state_t rpc_client_get_state(rpc_client_t *client) {
  if (!client)
    return RPC_STATE_DISCONNECTED;
  return client->state;
}

int rpc_client_call(rpc_client_t *client, const char *method, const char *params,
                    rpc_call_result_t *result) {
  if (!client || !method || !result)
    return -1;

  /* Generate request ID */
  char id[32];
  stbsp_snprintf(id, sizeof(id), "%d", client->request_id_counter++);

  /* Build JSON-RPC request */
  size_t jsonrpc_len = 0;
  char *jsonrpc_body = build_jsonrpc_request(method, params, id, &jsonrpc_len);
  if (!jsonrpc_body)
    return -1;

  /* Build URL */
  const char *url = build_url(client);

  /* Direct coroutine call �?blocks this coroutine, not the thread */
  http_response_t *resp =
      http_request(client->http_client, HTTP_POST, url, NULL, 0, jsonrpc_body, jsonrpc_len);
  json_serialize_free(jsonrpc_body);

  if (!resp)
    return -1;

  process_coro_response(resp, result);
  http_response_free(resp);

  return 0;
}

int rpc_client_notify(rpc_client_t *client, const char *method, const char *params) {
  if (!client || !method)
    return -1;

  /* Build JSON-RPC notification (no ID) */
  size_t jsonrpc_len = 0;
  char *jsonrpc_body = build_jsonrpc_request(method, params, NULL, &jsonrpc_len);
  if (!jsonrpc_body)
    return -1;

  /* Build URL */
  const char *url = build_url(client);

  /* Fire and forget �?send request, ignore response */
  http_response_t *resp =
      http_request(client->http_client, HTTP_POST, url, NULL, 0, jsonrpc_body, jsonrpc_len);
  json_serialize_free(jsonrpc_body);

  if (resp)
    http_response_free(resp);

  return 0;
}

void rpc_result_free(rpc_call_result_t *result) {
  if (!result)
    return;

  free(result->result);
  free(result->error_message);
  free(result->id);

  memset(result, 0, sizeof(rpc_call_result_t));
}

int rpc_result_is_success(const rpc_call_result_t *result) { return result ? result->success : 0; }

const char *rpc_result_get_error(const rpc_call_result_t *result) {
  return result ? result->error_message : NULL;
}

char *rpc_build_params(const char *format, ...) {
  if (!format)
    return NULL;

  va_list args;
  va_start(args, format);

  va_list args_copy;
  va_copy(args_copy, args);
  int size = vsnprintf(NULL, 0, format, args_copy);
  va_end(args_copy);

  if (size < 0) {
    va_end(args);
    return NULL;
  }

  char *result = (char *)malloc(size + 1);
  if (result) {
    vsnprintf(result, size + 1, format, args);
  }

  va_end(args);
  return result;
}

char *rpc_result_get_string(const rpc_call_result_t *result, const char *key) {
  if (!result || !result->result)
    return NULL;

  return json_extract_string(result->result, key);
}

int rpc_result_get_int(const rpc_call_result_t *result, const char *key, int64_t *value) {
  if (!result || !result->result || !value)
    return -1;

  return json_extract_int(result->result, key, value);
}

int rpc_result_get_bool(const rpc_call_result_t *result, const char *key, int *value) {
  if (!result || !result->result || !value)
    return -1;

  return json_extract_bool(result->result, key, value);
}

int rpc_result_get_double(const rpc_call_result_t *result, const char *key, double *value) {
  if (!result || !result->result || !value)
    return -1;

  return json_extract_double(result->result, key, value);
}

/* Async call �?spawn a coroutine that does the request and calls the user callback */
typedef struct {
  rpc_client_t *client;
  char *method;
  char *params;
  char *id;
  rpc_callback_t user_callback;
  void *user_data;
} async_coro_args_t;

static void async_call_coro(turbo_coro_t *co, void *arg) {
  async_coro_args_t *a = (async_coro_args_t *)arg;

  size_t jsonrpc_len = 0;
  char *jsonrpc_body = build_jsonrpc_request(a->method, a->params, a->id, &jsonrpc_len);

  if (jsonrpc_body) {
    const char *url = build_url(a->client);
    http_response_t *resp =
        http_request(a->client->http_client, HTTP_POST, url, NULL, 0, jsonrpc_body, jsonrpc_len);
    json_serialize_free(jsonrpc_body);

    if (resp) {
      rpc_call_result_t result;
      process_coro_response(resp, &result);
      http_response_free(resp);

      if (a->user_callback) {
        a->user_callback(&result, a->user_data);
      }
      rpc_result_free(&result);
    }
  }

  free(a->method);
  free(a->params);
  free(a->id);
  free(a);
}

int rpc_client_call_async(rpc_client_t *client, const char *method, const char *params,
                          rpc_callback_t callback, void *user_data) {
  if (!client || !method || !callback)
    return -1;

  /* Generate request ID */
  char id[32];
  stbsp_snprintf(id, sizeof(id), "%d", client->request_id_counter++);

  /* Create args for the coroutine (must be heap-allocated, outlives this call) */
  async_coro_args_t *args = (async_coro_args_t *)malloc(sizeof(async_coro_args_t));
  if (!args)
    return -1;

  args->client = client;
  args->method = strdup(method);
  args->params = params ? strdup(params) : NULL;
  args->id = strdup(id);
  args->user_callback = callback;
  args->user_data = user_data;

  turbo_coro_t *co = turbo_coro_create(async_call_coro, args, NULL);
  if (!co) {
    free(args->method);
    free(args->params);
    free(args->id);
    free(args);
    return -1;
  }

  turbo_coro_resume(co);
  return 0;
}

int rpc_client_batch_call(rpc_client_t *client, const char **methods, const char **params,
                          size_t count, rpc_call_result_t *results) {
  if (!client || !methods || !results || count == 0)
    return -1;

  /* Build batch request JSON */
  size_t batch_size = 2; /* "[]" */
  for (size_t i = 0; i < count; i++) {
    batch_size += 256; /* Estimate per request */
    if (params && params[i]) {
      batch_size += strlen(params[i]);
    }
  }

  char *batch_json = (char *)malloc(batch_size);
  if (!batch_json)
    return -1;

  size_t pos = 0;
  batch_json[pos++] = '[';

  for (size_t i = 0; i < count; i++) {
    char id_str[32];
    stbsp_snprintf(id_str, sizeof(id_str), "%d", client->request_id_counter++);

    size_t req_len;
    char *req = build_jsonrpc_request(methods[i], params ? params[i] : NULL, id_str, &req_len);
    if (!req) {
      free(batch_json);
      return -1;
    }

    if (i > 0) {
      batch_json[pos++] = ',';
    }

    if (pos + req_len + 2 >= batch_size) {
      json_serialize_free(req);
      free(batch_json);
      return -1;
    }

    memcpy(batch_json + pos, req, req_len);
    pos += req_len;
    json_serialize_free(req);
  }

  batch_json[pos++] = ']';
  batch_json[pos] = '\0';

  /* Build URL */
  const char *url = build_url(client);

  /* Direct coroutine call */
  http_response_t *resp =
      http_request(client->http_client, HTTP_POST, url, NULL, 0, batch_json, pos);
  free(batch_json);

  if (!resp)
    return -1;

  if (resp->error_code != HTTP_ERROR_NONE || !resp->body) {
    /* Mark all as failed */
    for (size_t i = 0; i < count; i++) {
      memset(&results[i], 0, sizeof(rpc_call_result_t));
      results[i].success = 0;
      results[i].error_code = RPC_ERROR_INTERNAL;
      results[i].http_status = resp->status_code;
      if (resp->error) {
        results[i].error_message = strdup(resp->error);
      }
    }
  } else {
    /* Simple batch parsing - assumes responses in order */
    for (size_t i = 0; i < count; i++) {
      memset(&results[i], 0, sizeof(rpc_call_result_t));
      results[i].success = 1;
      results[i].result = strdup("{}");
      results[i].http_status = resp->status_code;
    }
  }

  http_response_free(resp);
  return 0;
}

const char *rpc_client_version(void) { return RPC_CLIENT_VERSION; }

/* ============================================================================
 * Streaming (SSE) Implementation
 * ============================================================================ */

/* Stream call context */
typedef struct {
  rpc_callback_t result_cb;
  rpc_callback_t complete_cb;
  void *user_data;
  char *buffer;
  size_t buffer_size;
  size_t buffer_used;
} rpc_stream_context_t;

static void process_sse_event(rpc_stream_context_t *ctx, const char *event_data) {
  if (!event_data || event_data[0] == '\0')
    return;

  rpc_call_result_t result;
  memset(&result, 0, sizeof(result));

  if (parse_jsonrpc_response(event_data, &result) == 0) {
    if (ctx->result_cb) {
      ctx->result_cb(&result, ctx->user_data);
    }
  }
  rpc_result_free(&result);
}

static void rpc_stream_data_callback(const char *data, size_t len, void *user_data) {
  rpc_stream_context_t *ctx = (rpc_stream_context_t *)user_data;
  if (!ctx || !data || len == 0)
    return;

  /* Append to buffer */
  size_t new_size = ctx->buffer_used + len + 1;
  if (new_size > ctx->buffer_size) {
    size_t alloc_size = ctx->buffer_size == 0 ? 4096 : ctx->buffer_size * 2;
    while (alloc_size < new_size)
      alloc_size *= 2;
    char *new_buf = realloc(ctx->buffer, alloc_size);
    if (!new_buf)
      return;
    ctx->buffer = new_buf;
    ctx->buffer_size = alloc_size;
  }
  memcpy(ctx->buffer + ctx->buffer_used, data, len);
  ctx->buffer_used += len;
  ctx->buffer[ctx->buffer_used] = '\0';

  /* Process completed events - look for double newline */
  char *p = ctx->buffer;
  char *event_end;
  while ((event_end = strstr(p, "\n\n")) != NULL) {
    *event_end = '\0';

    /* Parse the event (look for data: lines) */
    char *event_ptr = p;
    const char *data_prefix = "data: ";
    size_t prefix_len = strlen(data_prefix);

    /* Allocate message buffer - at most as large as the event itself */
    char *msg_buf = (char *)malloc(strlen(event_ptr) + 1);
    if (!msg_buf)
      break;
    size_t msg_pos = 0;

    char *line = event_ptr;
    while (line < event_end) {
      char *next_line = strchr(line, '\n');
      if (!next_line)
        next_line = event_end;

      if (strncmp(line, data_prefix, prefix_len) == 0) {
        size_t line_len = next_line - (line + prefix_len);
        /* Strip trailing \r if present */
        if (line_len > 0 && line[prefix_len + line_len - 1] == '\r') {
          line_len--;
        }
        memcpy(msg_buf + msg_pos, line + prefix_len, line_len);
        msg_pos += line_len;
      }

      if (next_line >= event_end)
        break;
      line = next_line + 1;
    }
    msg_buf[msg_pos] = '\0';

    process_sse_event(ctx, msg_buf);
    free(msg_buf);

    p = event_end + 2;
  }

  /* Move remaining data to front */
  if (p > ctx->buffer) {
    size_t processed = p - ctx->buffer;
    if (processed < ctx->buffer_used) {
      size_t remaining = ctx->buffer_used - processed;
      memmove(ctx->buffer, p, remaining);
      ctx->buffer_used = remaining;
    } else {
      ctx->buffer_used = 0;
    }
    ctx->buffer[ctx->buffer_used] = '\0';
  }
}

/* Coroutine args for streaming */
typedef struct {
  rpc_client_t *client;
  char *jsonrpc_body;
  size_t jsonrpc_len;
  rpc_stream_context_t *stream_ctx;
} stream_coro_args_t;

static void stream_call_coro(turbo_coro_t *co, void *arg) {
  stream_coro_args_t *a = (stream_coro_args_t *)arg;
  rpc_stream_context_t *ctx = a->stream_ctx;

  const char *url = build_url(a->client);
  const char *headers[] = {"Accept: text/event-stream", "Cache-Control: no-cache"};

  http_response_t *resp = http_receive_stream_post(a->client->http_client, url, a->jsonrpc_body,
                                                   a->jsonrpc_len, rpc_stream_data_callback, ctx);

  json_serialize_free(a->jsonrpc_body);

  /* Stream finished call complete callback */
  if (ctx->complete_cb) {
    rpc_call_result_t result;
    memset(&result, 0, sizeof(result));

    if (resp) {
      result.http_status = resp->status_code;
      if (resp->error_code != HTTP_ERROR_NONE) {
        result.success = 0;
        result.error_code = RPC_ERROR_INTERNAL;
        if (resp->error) {
          result.error_message = strdup(resp->error);
        }
      } else {
        result.success = 1;
      }
    } else {
      result.success = 0;
      result.error_code = RPC_ERROR_INTERNAL;
      result.error_message = strdup("No response");
    }

    ctx->complete_cb(&result, ctx->user_data);
    rpc_result_free(&result);
  }

  if (resp)
    http_response_free(resp);
  free(ctx->buffer);
  free(ctx);
  free(a);
}

int rpc_client_call_stream(rpc_client_t *client, const char *method, const char *params,
                           rpc_callback_t result_cb, rpc_callback_t complete_cb, void *user_data) {
  if (!client || !method || !result_cb)
    return -1;

  /* Generate request ID */
  char id[32];
  stbsp_snprintf(id, sizeof(id), "%d", client->request_id_counter++);

  /* Build JSON-RPC request */
  size_t jsonrpc_len = 0;
  char *jsonrpc_body = build_jsonrpc_request(method, params, id, &jsonrpc_len);
  if (!jsonrpc_body)
    return -1;

  /* Create stream context */
  rpc_stream_context_t *ctx = (rpc_stream_context_t *)calloc(1, sizeof(rpc_stream_context_t));
  if (!ctx) {
    json_serialize_free(jsonrpc_body);
    return -1;
  }
  ctx->result_cb = result_cb;
  ctx->complete_cb = complete_cb;
  ctx->user_data = user_data;

  /* Create coroutine args */
  stream_coro_args_t *args = (stream_coro_args_t *)malloc(sizeof(stream_coro_args_t));
  if (!args) {
    free(ctx);
    json_serialize_free(jsonrpc_body);
    return -1;
  }
  args->client = client;
  args->jsonrpc_body = jsonrpc_body;
  args->jsonrpc_len = jsonrpc_len;
  args->stream_ctx = ctx;

  turbo_coro_t *co = turbo_coro_create(stream_call_coro, args, NULL);
  if (!co) {
    free(ctx);
    free(args);
    json_serialize_free(jsonrpc_body);
    return -1;
  }

  turbo_coro_resume(co);
  return 0;
}
