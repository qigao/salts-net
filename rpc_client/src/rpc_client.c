#include "rpc_client.h"
#include "rpc_error.h"
#include "tlog.h"
#include <fmt.h>
#include <http_client.h>
#include <json_parser.h>
#include <platform.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbo_str.h>
#include <turbo_coro.h>
#include <CoroNet/turbo_coro_context.h>


#define RPC_CLIENT_VERSION "1.0.0"
#define MAX_REQUEST_SIZE 65536

/* Client structure */
struct rpc_client_s {
  rpc_client_config_t config;
  rpc_client_state_t state;
  http_client_t *http_client;
  int owns_http_client;
  int request_id_counter;
};

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/* Build full URL from config */
static const char *build_url(rpc_client_t *client) { return client->config.url; }

static void rpc_result_set_message(rpc_call_result_t *result, int error_code, int http_status,
                                   const char *message) {
  if (!result)
    return;

  memset(result, 0, sizeof(rpc_call_result_t));
  result->success = 0;
  result->error_code = error_code;
  result->http_status = http_status;
  if (message)
    result->error_message = strdup(message);
}

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
    if (!params_obj) {
      json_free(root);
      return NULL;
    }
    json_object_add(root, "params", params_obj);
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
      if (!result->result) {
        result->success = 0;
        result->error_code = RPC_ERROR_INTERNAL;
        result->error_message = strdup("Failed to serialize JSON-RPC result");
      }
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
    if (parse_jsonrpc_response(resp->body, result) != 0) {
      result->success = 0;
      result->error_code = RPC_ERROR_PARSE;
      result->error_message = strdup("Invalid JSON-RPC response");
    }
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
  client->owns_http_client = 0;

  /* Set default headers */
  http_client_set_default_header(client->http_client, "Content-Type", "application/json");

  return client;
}

rpc_client_t *rpc_client_create_simple(const char *url) {
  rpc_client_t *client;
  http_client_t *http_client;
  rpc_client_config_t config;

  if (!url) {
    return NULL;
  }

  http_client = http_client_create(NULL);
  if (!http_client) {
    return NULL;
  }

  config.url = url;
  config.http_client = http_client;
  client = rpc_client_create(&config);
  if (!client) {
    http_client_destroy(http_client);
    return NULL;
  }

  client->owns_http_client = 1;
  return client;
}

void rpc_client_destroy(rpc_client_t *client) {
  if (!client)
    return;

  if (client->owns_http_client && client->http_client) {
    http_client_destroy(client->http_client);
    client->http_client = NULL;
  }

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
  fmt(id, sizeof(id), "{}", client->request_id_counter++);

  /* Build JSON-RPC request */
  size_t jsonrpc_len = 0;
  char *jsonrpc_body = build_jsonrpc_request(method, params, id, &jsonrpc_len);
  if (!jsonrpc_body)
    return -1;

  /* Build URL */
  const char *url = build_url(client);

  /* Direct coroutine call — blocks this coroutine, not the thread */
  http_response_t *resp =
      http_request(client->http_client, HTTP_POST, url, NULL, 0, jsonrpc_body, jsonrpc_len);
  json_serialize_free(jsonrpc_body);

  if (!resp)
    return -1;

  int call_rc = 0;
  if (resp->error_code != HTTP_ERROR_NONE || !resp->body || resp->body_len == 0)
    call_rc = -1;

  process_coro_response(resp, result);
  if (!result->success && result->error_code == RPC_ERROR_PARSE)
    call_rc = -1;
  http_response_free(resp);

  return call_rc;
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

  /* Fire and forget — send request, ignore response */
  http_response_t *resp =
      http_request(client->http_client, HTTP_POST, url, NULL, 0, jsonrpc_body, jsonrpc_len);
  json_serialize_free(jsonrpc_body);

  if (!resp)
    return -1;

  if (resp->error_code != HTTP_ERROR_NONE || resp->status_code >= 400) {
    http_response_free(resp);
    return -1;
  }

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

  tstr_t temp = tstr_new();

  if (!temp) {
    va_end(args);
    return NULL;
  }

  temp = tstr_cat_vfmt(temp, format, args);
  va_end(args);
  char *result = tstr_to_cstr(temp);
  tstr_free(temp);
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

int rpc_client_batch_call(rpc_client_t *client, const char **methods, const char **params,
                          size_t count, rpc_call_result_t *results) {
  if (!client || !methods || !results || count == 0)
    return -1;

  char **request_ids = (char **)calloc(count, sizeof(char *));
  if (!request_ids)
    return -1;

  /* Build batch request JSON (manual formatting for safety and efficiency) */
  size_t batch_size = 2; /* "[]" */
  for (size_t i = 0; i < count; i++) {
    batch_size += 256; /* Estimate per request structure */
    if (params && params[i]) {
      batch_size += strlen(params[i]);
    }
  }

  char *batch_json = (char *)malloc(batch_size);
  if (!batch_json) {
    free(request_ids);
    return -1;
  }

  size_t pos = 0;
  batch_json[pos++] = '[';

  for (size_t i = 0; i < count; i++) {
    char id_str[32];
    fmt(id_str, sizeof(id_str), "{}", client->request_id_counter++);
    request_ids[i] = strdup(id_str);
    if (!request_ids[i]) {
      for (size_t j = 0; j < i; j++)
        free(request_ids[j]);
      free(request_ids);
      free(batch_json);
      return -1;
    }

    size_t req_len;
    char *req = build_jsonrpc_request(methods[i], params ? params[i] : NULL, id_str, &req_len);
    if (!req) {
      for (size_t j = 0; j <= i; j++)
        free(request_ids[j]);
      free(request_ids);
      free(batch_json);
      return -1;
    }

    if (i > 0) {
      batch_json[pos++] = ',';
    }

    if (pos + req_len + 2 >= batch_size) {
      json_serialize_free(req);
      for (size_t j = 0; j <= i; j++)
        free(request_ids[j]);
      free(request_ids);
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
    goto batch_fail;

  if (resp->error_code != HTTP_ERROR_NONE || !resp->body) {
    /* Mark all as failed */
    for (size_t i = 0; i < count; i++) {
      rpc_result_set_message(&results[i], RPC_ERROR_INTERNAL, resp->status_code, resp->error);
    }
    http_response_free(resp);
    for (size_t i = 0; i < count; i++)
      free(request_ids[i]);
    free(request_ids);
    return -1;
  }

  json_value_t *root = json_parse(resp->body, resp->body_len);
  if (!root || json_type(root) != JSON_ARRAY)
    goto batch_parse_fail;

  for (size_t i = 0; i < count; i++)
    memset(&results[i], 0, sizeof(rpc_call_result_t));

  size_t response_count = json_array_size(root);
  int parse_ok = (response_count == count);
  int *matched = (int *)calloc(count, sizeof(int));
  if (!matched) {
    json_free(root);
    goto batch_fail;
  }

  for (size_t i = 0; i < response_count; i++) {
    json_value_t *item = json_array_get(root, i);
    size_t item_len = 0;
    char *item_json = json_serialize(item, &item_len);
    rpc_call_result_t parsed = {0};
    int matched_idx = -1;

    if (!item_json || parse_jsonrpc_response(item_json, &parsed) != 0 || !parsed.id) {
      parse_ok = 0;
      if (item_json)
        json_serialize_free(item_json);
      rpc_result_free(&parsed);
      continue;
    }

    json_serialize_free(item_json);

    for (size_t j = 0; j < count; j++) {
      if (strcmp(request_ids[j], parsed.id) == 0) {
        matched_idx = (int)j;
        break;
      }
    }

    if (matched_idx < 0 || matched[matched_idx]) {
      parse_ok = 0;
      rpc_result_free(&parsed);
      continue;
    }

    matched[matched_idx] = 1;
    parsed.http_status = resp->status_code;
    results[matched_idx] = parsed;
  }

  for (size_t i = 0; i < count; i++) {
    if (!matched[i]) {
      parse_ok = 0;
      rpc_result_free(&results[i]);
      rpc_result_set_message(&results[i], RPC_ERROR_PARSE, resp->status_code,
                             "Missing batch response item");
    }
  }

  free(matched);
  json_free(root);
  http_response_free(resp);
  for (size_t i = 0; i < count; i++)
    free(request_ids[i]);
  free(request_ids);

  return parse_ok ? 0 : -1;

batch_parse_fail:
  if (root)
    json_free(root);
  for (size_t i = 0; i < count; i++)
    rpc_result_set_message(&results[i], RPC_ERROR_PARSE, resp->status_code,
                           "Invalid JSON-RPC batch response");
  http_response_free(resp);
  for (size_t i = 0; i < count; i++)
    free(request_ids[i]);
  free(request_ids);
  return -1;

batch_fail:
  if (resp)
    http_response_free(resp);
  for (size_t i = 0; i < count; i++)
    free(request_ids[i]);
  free(request_ids);
  return -1;
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
  int completed;
  int failed;
  int error_code;
  int keep_until_caller;
  char *error_message;
} rpc_stream_context_t;

static void rpc_stream_fail(rpc_stream_context_t *ctx, int error_code, const char *message) {
  if (!ctx || ctx->failed)
    return;

  ctx->failed = 1;
  ctx->error_code = error_code;
  if (message)
    ctx->error_message = strdup(message);
}

static char *find_sse_event_end(char *buffer) {
  char *lf_end = strstr(buffer, "\n\n");
  char *crlf_end = strstr(buffer, "\r\n\r\n");

  if (!lf_end)
    return crlf_end;
  if (!crlf_end)
    return lf_end;
  return lf_end < crlf_end ? lf_end : crlf_end;
}

static void process_sse_event(rpc_stream_context_t *ctx, const char *event_data) {
  if (!event_data || event_data[0] == '\0')
    return;

  rpc_call_result_t result;
  memset(&result, 0, sizeof(result));

  if (parse_jsonrpc_response(event_data, &result) != 0) {
    result.success = 0;
    result.error_code = RPC_ERROR_PARSE;
    result.error_message = strdup("Invalid JSON-RPC response");
  }

  if (ctx->result_cb) {
    ctx->result_cb(&result, ctx->user_data);
  }
  rpc_result_free(&result);
}

static void rpc_stream_data_callback(const char *data, size_t len, void *user_data) {
  rpc_stream_context_t *ctx = (rpc_stream_context_t *)user_data;
  if (!ctx || !data || len == 0 || ctx->failed)
    return;
  /* Append to buffer */
  size_t new_size = ctx->buffer_used + len + 1;
  if (new_size > ctx->buffer_size) {
    size_t alloc_size = ctx->buffer_size == 0 ? 4096 : ctx->buffer_size * 2;
    while (alloc_size < new_size)
      alloc_size *= 2;
    char *new_buf = realloc(ctx->buffer, alloc_size);
    if (!new_buf) {
      rpc_stream_fail(ctx, RPC_ERROR_OUT_OF_MEMORY, "SSE buffer allocation failed");
      return;
    }
    ctx->buffer = new_buf;
    ctx->buffer_size = alloc_size;
  }
  memcpy(ctx->buffer + ctx->buffer_used, data, len);
  ctx->buffer_used += len;
  ctx->buffer[ctx->buffer_used] = '\0';

  /* Process completed events - support LF and CRLF framing. */
  char *p = ctx->buffer;
  char *event_end;
  while (*p == '\r' || *p == '\n') {
    p++;
  }

  while ((event_end = find_sse_event_end(p)) != NULL) {
    size_t terminator_len = (event_end[0] == '\r') ? 4 : 2;
    *event_end = '\0';

    /* Parse the event (look for data: lines) */
    char *event_ptr = p;
    const char *data_prefix = "data: ";
    size_t prefix_len = strlen(data_prefix);

    /* Allocate message buffer - at most as large as the event itself */
    char *msg_buf = (char *)malloc(strlen(event_ptr) + 1);
    if (!msg_buf) {
      rpc_stream_fail(ctx, RPC_ERROR_OUT_OF_MEMORY, "SSE event allocation failed");
      break;
    }
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

    p = event_end + terminator_len;
    while (*p == '\r' || *p == '\n') {
      p++;
    }
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

static void stream_call_coro(coro_t *co, void *arg) {
  UNUSED(co);
  stream_coro_args_t *a = (stream_coro_args_t *)arg;
  rpc_stream_context_t *ctx = a->stream_ctx;

  const char *url = build_url(a->client);

  http_response_t *resp = http_receive_stream_post(a->client->http_client, url, a->jsonrpc_body,
                                                   a->jsonrpc_len, rpc_stream_data_callback, ctx);

  json_serialize_free(a->jsonrpc_body);

  if (!ctx->failed) {
    if (!resp) {
      ctx->failed = 1;
      ctx->error_code = RPC_ERROR_INTERNAL;
      if (!ctx->error_message) {
        ctx->error_message = strdup("No response");
      }
    } else if (resp->error_code != HTTP_ERROR_NONE) {
      ctx->failed = 1;
      ctx->error_code = RPC_ERROR_INTERNAL;
      if (!ctx->error_message && resp->error) {
        ctx->error_message = strdup(resp->error);
      }
    }
  }

  /* Stream finished call complete callback */
  if (ctx->complete_cb) {
    rpc_call_result_t result;
    memset(&result, 0, sizeof(result));

    if (resp) {
      result.http_status = resp->status_code;
      if (ctx->failed) {
        result.success = 0;
        result.error_code = ctx->error_code ? ctx->error_code : RPC_ERROR_INTERNAL;
        if (ctx->error_message) {
          result.error_message = strdup(ctx->error_message);
        }
      } else {
        result.success = 1;
      }
    } else {
      result.success = 0;
      result.error_code = RPC_ERROR_INTERNAL;
      result.error_message = strdup("No response");
    }

    ctx->completed = 1;
    ctx->complete_cb(&result, ctx->user_data);
    rpc_result_free(&result);
  } else {
    ctx->completed = 1;
  }

  if (resp)
    http_response_free(resp);

  if (ctx->keep_until_caller) {
    free(ctx->error_message);
    ctx->error_message = NULL;
    free(ctx->buffer);
    ctx->buffer = NULL;
  } else {
    free(ctx->error_message);
    free(ctx->buffer);
    free(ctx);
  }
  free(a);
}

int rpc_client_call_stream(rpc_client_t *client, const char *method, const char *params,
                           rpc_callback_t result_cb, rpc_callback_t complete_cb, void *user_data) {
  coro_context_t *coro_ctx;
  int in_coro;

  if (!client || !method || !result_cb)
    return -1;

  /* Generate request ID */
  char id[32];
  fmt(id, sizeof(id), "{}", client->request_id_counter++);

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
  in_coro = coro_running();
  ctx->keep_until_caller = !in_coro;

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

  /* Use context API */
  coro_ctx = http_client_get_context(client->http_client);
  if (!coro_ctx)
    coro_ctx = coro_context_current();

  if (!coro_ctx) {
    free(ctx);
    free(args);
    json_serialize_free(jsonrpc_body);
    return -1;
  }

  if (coro_context_spawn(coro_ctx, stream_call_coro, args) != 0) {
    free(ctx);
    free(args);
    json_serialize_free(jsonrpc_body);
    return -1;
  }

  if (in_coro) {
    return 0;
  }

  while (!ctx->completed && coro_context_alive(coro_ctx)) {
    coro_context_run(coro_ctx, TURBO_RUN_ONCE);
  }

  int rc = (ctx->completed && !ctx->failed) ? 0 : -1;
  free(ctx->error_message);
  free(ctx->buffer);
  free(ctx);
  return rc;
}
