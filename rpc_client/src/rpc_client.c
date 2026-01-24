#include "rpc_client.h"
#include "tlog.h"
#include "rpc_error.h"
#include "cjson/cJSON.h"
#include <http_client_async.h>
#include <platform.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stb_sprintf.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>

#endif

#define RPC_CLIENT_VERSION "1.0.0"
#define MAX_REQUEST_SIZE 65536

/* Client structure */
struct rpc_client_s {
  rpc_client_config_t config;
  rpc_client_state_t state;
  http_async_client_t *http_client;
  int request_id_counter;
};

/* Async call context */
typedef struct {
  rpc_callback_t user_callback;
  void *user_data;
  rpc_call_result_t *result;
} async_call_context_t;

/* Stream call context */
typedef struct {
  rpc_callback_t result_cb;
  rpc_callback_t complete_cb;
  void *user_data;
  char *buffer;
  size_t buffer_size;
  size_t buffer_used;
} rpc_stream_context_t;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/* Build full URL from config */
static const char *build_url(rpc_client_t *client) {
  return client->config.url;
}

/* Build JSON-RPC request using cJSON */
static char *build_jsonrpc_request(const char *method, const char *params, const char *id,
                                   size_t *request_len) {
  if (!method || !request_len)
    return NULL;

  cJSON *root = cJSON_CreateObject();
  if (!root)
    return NULL;

  cJSON_AddStringToObject(root, "jsonrpc", "2.0");
  cJSON_AddStringToObject(root, "method", method);

  /* Add params if provided */
  if (params && params[0] != '\0') {
    cJSON *params_obj = cJSON_Parse(params);
    if (params_obj) {
      cJSON_AddItemToObject(root, "params", params_obj);
    }
  }

  /* Add ID if provided (regular request), otherwise it's a notification */
  if (id) {
    cJSON_AddStringToObject(root, "id", id);
  }

  char *request = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (!request)
    return NULL;

  *request_len = strlen(request);
  return request;
}

/* JSON value extraction using cJSON */
static char *json_extract_string(const char *json, const char *key) {
  if (!json)
    return NULL;

  cJSON *root = cJSON_Parse(json);
  if (!root)
    return NULL;

  char *result = NULL;

  if (key) {
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsString(item)) {
      result = strdup(item->valuestring);
    }
  } else {
    /* Root value */
    if (cJSON_IsString(root)) {
      result = strdup(root->valuestring);
    }
  }

  cJSON_Delete(root);
  return result;
}

static int json_extract_int(const char *json, const char *key, int64_t *value) {
  if (!json || !value)
    return -1;

  cJSON *root = cJSON_Parse(json);
  if (!root)
    return -1;

  int ret = -1;

  if (key) {
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsNumber(item)) {
      *value = (int64_t)item->valuedouble;
      ret = 0;
    }
  } else {
    if (cJSON_IsNumber(root)) {
      *value = (int64_t)root->valuedouble;
      ret = 0;
    }
  }

  cJSON_Delete(root);
  return ret;
}

static int json_extract_bool(const char *json, const char *key, int *value) {
  if (!json || !value)
    return -1;

  cJSON *root = cJSON_Parse(json);
  if (!root)
    return -1;

  int ret = -1;

  if (key) {
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsBool(item)) {
      *value = cJSON_IsTrue(item) ? 1 : 0;
      ret = 0;
    }
  } else {
    if (cJSON_IsBool(root)) {
      *value = cJSON_IsTrue(root) ? 1 : 0;
      ret = 0;
    }
  }

  cJSON_Delete(root);
  return ret;
}

static int json_extract_double(const char *json, const char *key, double *value) {
  if (!json || !value)
    return -1;

  cJSON *root = cJSON_Parse(json);
  if (!root)
    return -1;

  int ret = -1;

  if (key) {
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsNumber(item)) {
      *value = item->valuedouble;
      ret = 0;
    }
  } else {
    if (cJSON_IsNumber(root)) {
      *value = root->valuedouble;
      ret = 0;
    }
  }

  cJSON_Delete(root);
  return ret;
}

/* Parse JSON-RPC response using cJSON */
static int parse_jsonrpc_response(const char *body, rpc_call_result_t *result) {
  if (!body || !result)
    return -1;

  /* Note: caller should memset result before calling this function */

  cJSON *root = cJSON_Parse(body);
  if (!root)
    return -1;

  /* Extract ID */
  cJSON *id_item = cJSON_GetObjectItem(root, "id");
  if (id_item && cJSON_IsString(id_item)) {
    result->id = strdup(id_item->valuestring);
  }

  /* Check for error */
  cJSON *error = cJSON_GetObjectItem(root, "error");
  if (error) {
    result->success = 0;

    /* Extract error code */
    cJSON *code = cJSON_GetObjectItem(error, "code");
    if (code && cJSON_IsNumber(code)) {
      result->error_code = (int)code->valuedouble;
    }

    /* Extract error message */
    cJSON *message = cJSON_GetObjectItem(error, "message");
    if (message && cJSON_IsString(message)) {
      result->error_message = strdup(message->valuestring);
    }
  } else {
    result->success = 1;

    /* Extract result */
    cJSON *result_item = cJSON_GetObjectItem(root, "result");
    if (result_item) {
      result->result = cJSON_PrintUnformatted(result_item);
    }
  }

  cJSON_Delete(root);
  return 0;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

rpc_client_t *rpc_client_create(const rpc_client_config_t *config) {
  if (!config || !config->url)
    return NULL;

  rpc_client_t *client = (rpc_client_t *)calloc(1, sizeof(rpc_client_t));
  if (!client)
    return NULL;

  client->config = *config;
  client->state = RPC_STATE_DISCONNECTED;
  client->request_id_counter = 1;

  /* Create HTTP client */
  client->http_client = http_async_client_create();
  if (!client->http_client) {
    free(client);
    return NULL;
  }

  /* Configure HTTP client */
  if (config->timeout_ms > 0) {
    http_async_client_set_timeout(client->http_client, config->timeout_ms);
  }

  if (config->user_agent) {
    http_async_client_set_user_agent(client->http_client, config->user_agent);
  }

  /* Set default headers */
  http_async_client_set_default_header(client->http_client, "Content-Type", "application/json");

  if (config->keep_alive) {
    http_async_client_set_default_header(client->http_client, "Connection", "keep-alive");
  }

  return client;
}

void rpc_client_destroy(rpc_client_t *client) {
  if (!client)
    return;

  if (client->http_client) {
    http_async_client_destroy(client->http_client);
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

/* Synchronous call helper - uses async internally with blocking */
typedef struct {
  int done;
  rpc_call_result_t *result;
} sync_call_context_t;

static void sync_call_callback(http_async_request_t *request, http_async_response_t *response,
                               void *user_data) {
  (void)request;
  sync_call_context_t *ctx = (sync_call_context_t *)user_data;

  TLOG_DEBUG("sync_call_callback called (ctx={}, response={})", (void*)ctx, (void*)response);

  if (!ctx || !ctx->result) {
    TLOG_DEBUG("ERROR: Invalid ctx or result");
    return;
  }

  TLOG_DEBUG("HTTP status: {}, error_code: {}, body_len: {}",
         (int)response->status_code, (int)response->error_code, (unsigned long long)response->body_len);

  memset(ctx->result, 0, sizeof(rpc_call_result_t));

  if (response->error_code != HTTP_ASYNC_ERROR_NONE) {
    /* Network/HTTP error */
    TLOG_DEBUG("HTTP error detected");
    ctx->result->success = 0;
    ctx->result->error_code = RPC_ERROR_INTERNAL;
    if (response->error) {
      ctx->result->error_message = strdup(response->error);
    }
  } else if (response->body && response->body_len > 0) {
    /* Parse JSON-RPC response */
    TLOG_DEBUG("Parsing JSON-RPC response ({} bytes)", (unsigned long long)response->body_len);
    parse_jsonrpc_response(response->body, ctx->result);
  } else {
    /* Empty response */
    TLOG_DEBUG("Empty response");
    ctx->result->success = 0;
    ctx->result->error_code = RPC_ERROR_INTERNAL;
    ctx->result->error_message = strdup("Empty response");
  }

  /* Set HTTP status AFTER parsing (parse may memset the result) */
  ctx->result->http_status = response->status_code;

  TLOG_DEBUG("Setting ctx->done = 1");
  ctx->done = 1;
  TLOG_DEBUG("sync_call_callback finished");
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

  /* Create sync context */
  sync_call_context_t ctx = {0, result};

  /* Make async HTTP POST request (Content-Type already set as default header) */
  http_async_request_t *req = http_async_request(client->http_client, HTTP_POST, url,
                                                  NULL, 0, jsonrpc_body, jsonrpc_len,
                                                  sync_call_callback, &ctx);
  free(jsonrpc_body);

  if (!req)
    return -1;

  /* Wait for completion - async_client runs its own thread with libuv event loop */
  while (!ctx.done) {
    /* Sleep briefly to avoid busy-wait CPU burn */
    #ifdef _WIN32
    Sleep(1); /* 1ms sleep on Windows */
    #else
    usleep(1000); /* 1ms sleep on Unix */
    #endif
  }

  return 0;
}

static void notify_callback(http_async_request_t *request, http_async_response_t *response,
                            void *user_data) {
  (void)request;
  (void)response;
  (void)user_data;
  /* Notifications don't care about responses */
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

  /* Make async HTTP POST request (fire and forget, Content-Type already set as default header) */
  http_async_request_t *req = http_async_request(client->http_client, HTTP_POST, url,
                                                  NULL, 0, jsonrpc_body, jsonrpc_len,
                                                  notify_callback, NULL);
  free(jsonrpc_body);

  return req ? 0 : -1;
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

static void async_call_callback(http_async_request_t *request, http_async_response_t *response,
                                void *user_data) {
  (void)request;
  async_call_context_t *ctx = (async_call_context_t *)user_data;

  if (!ctx || !ctx->result)
    return;

  memset(ctx->result, 0, sizeof(rpc_call_result_t));
  ctx->result->http_status = response->status_code;

  if (response->error_code != HTTP_ASYNC_ERROR_NONE) {
    /* Network/HTTP error */
    ctx->result->success = 0;
    ctx->result->error_code = RPC_ERROR_INTERNAL;
    if (response->error) {
      ctx->result->error_message = strdup(response->error);
    }
  } else if (response->body && response->body_len > 0) {
    /* Parse JSON-RPC response */
    parse_jsonrpc_response(response->body, ctx->result);
  }

  /* Call user callback */
  if (ctx->user_callback) {
    ctx->user_callback(ctx->result, ctx->user_data);
  }

  /* Cleanup */
  rpc_result_free(ctx->result);
  free(ctx->result);
  free(ctx);
}

int rpc_client_call_async(rpc_client_t *client, const char *method, const char *params,
                          rpc_callback_t callback, void *user_data) {
  if (!client || !method || !callback)
    return -1;

  /* Generate request ID */
  char id[32];
  stbsp_snprintf(id, sizeof(id), "%d", client->request_id_counter++);

  /* Build JSON-RPC request */
  size_t jsonrpc_len = 0;
  char *jsonrpc_body = build_jsonrpc_request(method, params, id, &jsonrpc_len);
  if (!jsonrpc_body)
    return -1;

  /* Create async context */
  async_call_context_t *ctx = (async_call_context_t *)malloc(sizeof(async_call_context_t));
  if (!ctx) {
    free(jsonrpc_body);
    return -1;
  }

  ctx->user_callback = callback;
  ctx->user_data = user_data;
  ctx->result = (rpc_call_result_t *)calloc(1, sizeof(rpc_call_result_t));
  if (!ctx->result) {
    free(ctx);
    free(jsonrpc_body);
    return -1;
  }

  /* Build URL */
  const char *url = build_url(client);

  /* Make async HTTP POST request (Content-Type already set as default header) */
  http_async_request_t *req = http_async_request(client->http_client, HTTP_POST, url,
                                                  NULL, 0, jsonrpc_body, jsonrpc_len,
                                                  async_call_callback, ctx);
  free(jsonrpc_body);

  if (!req) {
    free(ctx->result);
    free(ctx);
    return -1;
  }

  return 0;
}

typedef struct {
  int done;
  rpc_call_result_t *results;
  size_t count;
} batch_call_context_t;

static void batch_call_callback(http_async_request_t *request, http_async_response_t *response,
                                void *user_data) {
  (void)request;
  batch_call_context_t *ctx = (batch_call_context_t *)user_data;

  if (!ctx || !ctx->results)
    return;

  /* For simplicity, parse batch response as array and extract results */
  /* A full implementation would properly parse the JSON array */
  if (response->error_code != HTTP_ASYNC_ERROR_NONE || !response->body) {
    /* Mark all as failed */
    for (size_t i = 0; i < ctx->count; i++) {
      memset(&ctx->results[i], 0, sizeof(rpc_call_result_t));
      ctx->results[i].success = 0;
      ctx->results[i].error_code = RPC_ERROR_INTERNAL;
      ctx->results[i].http_status = response->status_code;
      if (response->error) {
        ctx->results[i].error_message = strdup(response->error);
      }
    }
  } else {
    /* Simple batch parsing - assumes responses in order */
    /* TODO: Implement proper JSON array parsing */
    for (size_t i = 0; i < ctx->count; i++) {
      memset(&ctx->results[i], 0, sizeof(rpc_call_result_t));
      ctx->results[i].success = 1;
      ctx->results[i].result = strdup("{}");
      ctx->results[i].http_status = response->status_code;
    }
  }

  ctx->done = 1;
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
      free(req);
      free(batch_json);
      return -1;
    }

    memcpy(batch_json + pos, req, req_len);
    pos += req_len;
    free(req);
  }

  batch_json[pos++] = ']';
  batch_json[pos] = '\0';

  /* Build URL */
  const char *url = build_url(client);

  /* Create batch context */
  batch_call_context_t ctx = {0, results, count};

  /* Make async HTTP POST request (Content-Type already set as default header) */
  http_async_request_t *req = http_async_request(client->http_client, HTTP_POST, url,
                                                  NULL, 0, batch_json, pos,
                                                  batch_call_callback, &ctx);
  free(batch_json);

  if (!req)
    return -1;

  /* Wait for completion - async_client runs its own thread with libuv event loop */
  while (!ctx.done) {
    /* Sleep briefly to avoid busy-wait CPU burn */
    #ifdef _WIN32
    Sleep(1); /* 1ms sleep on Windows */
    #else
    usleep(1000); /* 1ms sleep on Unix */
    #endif
  }

  return 0;
}

const char *rpc_client_version(void) { return RPC_CLIENT_VERSION; }

/* ============================================================================
 * Streaming (SSE) Implementation
 * ========================================================================= */

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

static void rpc_stream_data_callback(http_async_request_t *request, const char *data, size_t len,
                                     void *user_data) {
  (void)request;
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
    if (!msg_buf) break;
    size_t msg_pos = 0;

    char *line = event_ptr;
    while (line < event_end) {
      char *next_line = strchr(line, '\n');
      if (!next_line)
        next_line = event_end;

      /* Trim leading \r if present */
      if (next_line > line && *(next_line - 1) == '\r') {
          // Adjust for CRLF if needed, but the prefix check will handle it
      }

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

static void rpc_stream_complete_callback(http_async_request_t *request,
                                         http_async_response_t *response, void *user_data) {
  (void)request;
  rpc_stream_context_t *ctx = (rpc_stream_context_t *)user_data;
  if (!ctx)
    return;

  if (ctx->complete_cb) {
    rpc_call_result_t result;
    memset(&result, 0, sizeof(result));
    result.http_status = response->status_code;

    if (response->error_code != HTTP_ASYNC_ERROR_NONE) {
      result.success = 0;
      result.error_code = RPC_ERROR_INTERNAL;
      if (response->error) {
        result.error_message = strdup(response->error);
      }
    } else {
      result.success = 1;
    }

    ctx->complete_cb(&result, ctx->user_data);
    rpc_result_free(&result);
  }

  /* Cleanup context */
  free(ctx->buffer);
  free(ctx);
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
    free(jsonrpc_body);
    return -1;
  }
  ctx->result_cb = result_cb;
  ctx->complete_cb = complete_cb;
  ctx->user_data = user_data;

  /* Build URL */
  const char *url = build_url(client);

  /* Make async HTTP POST request (Content-Type already set as default header) */
  const char *headers[] = {"Accept: text/event-stream", "Cache-Control: no-cache"};

  http_async_request_t *req = http_async_request(client->http_client, HTTP_POST, url, headers, 2,
                                                  jsonrpc_body, jsonrpc_len,
                                                  rpc_stream_complete_callback, ctx);
  free(jsonrpc_body);

  if (!req) {
    free(ctx);
    return -1;
  }

  /* Enable streaming mode in HTTP client to prevent memory accumulation */
  http_async_request_set_stream_only(req, 1);
  http_async_request_set_data_callback(req, rpc_stream_data_callback, ctx);

  return 0;
}
