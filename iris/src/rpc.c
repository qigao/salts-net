#include "rpc.h"
#include "iris.h"
#include "security.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include "tlog.h"
#include <stb_sprintf.h>

/* Global RPC context - temporary solution until iris_app_t refactor */
static rpc_context_t *g_rpc_context = NULL;

/* Cached params object to avoid re-parsing */
typedef struct {
  cJSON *params_obj;
  int cached;
} rpc_params_cache_t;

rpc_context_t *rpc_init(const rpc_config_t *config) {
  if (!config)
    return NULL;

  rpc_context_t *ctx = (rpc_context_t *)calloc(1, sizeof(rpc_context_t));
  if (!ctx)
    return NULL;

  // Copy config values
  ctx->config = *config;
  
  // Duplicate the endpoint string to avoid dangling pointer issues
  if (config->endpoint) {
    ctx->config.endpoint = strdup(config->endpoint);
    if (!ctx->config.endpoint) {
      free(ctx);
      return NULL;
    }
  }

  ctx->method_capacity = 16;
  ctx->methods = (rpc_method_t *)calloc(ctx->method_capacity, sizeof(rpc_method_t));
  if (!ctx->methods) {
    // Fix memory leak: free the duplicated endpoint string
    if (ctx->config.endpoint) {
      free((void*)ctx->config.endpoint);
    }
    free(ctx);
    return NULL;
  }

  ctx->method_count = 0;
  ctx->arena = NULL; /* Will be created per-request */

  return ctx;
}

void rpc_destroy(rpc_context_t *ctx) {
  if (!ctx)
    return;

  /* Clear global reference if this is the active context */
  if (g_rpc_context == ctx) {
    g_rpc_context = NULL;
  }

  if (ctx->methods)
    free(ctx->methods);

  /* Free the duplicated endpoint string */
  if (ctx->config.endpoint) {
    free((void*)ctx->config.endpoint);
  }

  free(ctx);
}

int rpc_register_method(rpc_context_t *ctx, const rpc_method_t *method) {
  if (!ctx || !method || !method->name || !method->handler)
    return -1;

  /* Check if method already exists */
  for (size_t i = 0; i < ctx->method_count; i++) {
    if (strcmp(ctx->methods[i].name, method->name) == 0)
      return -1; /* Already registered */
  }

  /* Expand capacity if needed */
  if (ctx->method_count >= ctx->method_capacity) {
    size_t new_capacity = ctx->method_capacity * 2;
    rpc_method_t *new_methods =
        (rpc_method_t *)realloc(ctx->methods, new_capacity * sizeof(rpc_method_t));
    if (!new_methods)
      return -1;

    ctx->methods = new_methods;
    ctx->method_capacity = new_capacity;
  }

  /* Add method */
  ctx->methods[ctx->method_count++] = *method;
  return 0;
}

int rpc_unregister_method(rpc_context_t *ctx, const char *method_name) {
  if (!ctx || !method_name)
    return -1;

  for (size_t i = 0; i < ctx->method_count; i++) {
    if (strcmp(ctx->methods[i].name, method_name) == 0) {
      /* Shift remaining methods */
      for (size_t j = i; j < ctx->method_count - 1; j++) {
        ctx->methods[j] = ctx->methods[j + 1];
      }
      ctx->method_count--;
      return 0;
    }
  }

  return -1; /* Not found */
}

int rpc_parse_request(Req *req, rpc_request_t *rpc_req) {
  if (!req || !rpc_req || !req->body)
    return RPC_ERROR_INVALID_REQUEST;

  /* Initialize RPC request */
  memset(rpc_req, 0, sizeof(rpc_request_t));
  rpc_req->arena = req->arena;
  rpc_req->protocol = RPC_PROTOCOL_JSON;

  /* Parse JSON */
  cJSON *root = cJSON_Parse(req->body);
  if (!root)
    return RPC_ERROR_PARSE;

  /* Extract jsonrpc version */
  cJSON *jsonrpc = cJSON_GetObjectItem(root, "jsonrpc");
  if (jsonrpc && cJSON_IsString(jsonrpc)) {
    rpc_req->jsonrpc = turbo_arena_strdup(req->arena, jsonrpc->valuestring);
  }

  /* Extract method (required) */
  cJSON *method = cJSON_GetObjectItem(root, "method");
  if (!method || !cJSON_IsString(method)) {
    cJSON_Delete(root);
    return RPC_ERROR_INVALID_REQUEST;
  }
  rpc_req->method = turbo_arena_strdup(req->arena, method->valuestring);

  /* Extract params (optional) */
  cJSON *params = cJSON_GetObjectItem(root, "params");
  if (params) {
    char *params_str = cJSON_PrintUnformatted(params);
    if (params_str) {
      rpc_req->params = turbo_arena_strdup(req->arena, params_str);
      cJSON_free(params_str);
    }
  }

  /* Extract id (optional for notifications) */
  cJSON *id = cJSON_GetObjectItem(root, "id");
  if (id) {
    if (cJSON_IsString(id)) {
      /* Phase IRIS-1: Replace arena_sprintf with turbo_arena_alloc + stbsp_snprintf */
      size_t len = strlen(id->valuestring) + 3; /* quotes + null */
      rpc_req->id = turbo_arena_alloc(req->arena, len);
      if (rpc_req->id) {
        stbsp_snprintf((char *)rpc_req->id, len, "\"%s\"", id->valuestring);
      }
    } else if (cJSON_IsNumber(id)) {
      /* Phase IRIS-1: Replace arena_sprintf with turbo_arena_alloc + stbsp_snprintf */
      size_t len = 32; /* enough for int */
      rpc_req->id = turbo_arena_alloc(req->arena, len);
      if (rpc_req->id) {
        stbsp_snprintf((char *)rpc_req->id, len, "%d", id->valueint);
      }
    } else if (cJSON_IsNull(id)) {
      rpc_req->id = turbo_arena_strdup(req->arena, "null");
    }
  }

  cJSON_Delete(root);
  return 0;
}

int rpc_build_response(rpc_response_t *rpc_res, char **output, size_t *output_len) {
  if (!rpc_res || !output || !output_len)
    return -1;

  /* Phase IRIS-1: Updated to use turbo_arena_t */
  turbo_arena_t *arena = rpc_res->arena;
  if (!arena)
    return -1;

  /* Build JSON-RPC response using cJSON */
  cJSON *root = cJSON_CreateObject();
  if (!root)
    return -1;

  cJSON_AddStringToObject(root, "jsonrpc", "2.0");

  if (rpc_res->error_code != 0) {
    /* Error response */
    cJSON *error = cJSON_CreateObject();
    cJSON_AddNumberToObject(error, "code", rpc_res->error_code);
    
    // Escape error message to prevent injection
    if (rpc_res->error_message) {
      char *escaped_message = malloc(strlen(rpc_res->error_message) * 2 + 256);
      if (escaped_message) {
        iris_security_result_t escape_result = iris_escape_json(rpc_res->error_message, escaped_message, strlen(rpc_res->error_message) * 2 + 256);
        if (escape_result == IRIS_SECURITY_OK) {
          cJSON_AddStringToObject(error, "message", escaped_message);
        } else {
          // Fallback to original message if escaping fails
          TLOG_WARN("Failed to escape RPC error message: {:s}", iris_security_error_string(escape_result));
          cJSON_AddStringToObject(error, "message", rpc_res->error_message);
        }
        free(escaped_message);
      } else {
        cJSON_AddStringToObject(error, "message", rpc_res->error_message);
      }
    } else {
      cJSON_AddStringToObject(error, "message", "Unknown error");
    }
    
    cJSON_AddItemToObject(root, "error", error);
  } else {
    /* Success response */
    if (rpc_res->result) {
      cJSON *result = cJSON_Parse(rpc_res->result);
      if (result) {
        cJSON_AddItemToObject(root, "result", result);
      } else {
        // If result is not valid JSON, escape it as a string
        char *escaped_result = malloc(strlen(rpc_res->result) * 2 + 256);
        if (escaped_result) {
          iris_security_result_t escape_result = iris_escape_json(rpc_res->result, escaped_result, strlen(rpc_res->result) * 2 + 256);
          if (escape_result == IRIS_SECURITY_OK) {
            cJSON_AddStringToObject(root, "result", escaped_result);
          } else {
            // Fallback to original result if escaping fails
            TLOG_WARN("Failed to escape RPC result: {:s}", iris_security_error_string(escape_result));
            cJSON_AddStringToObject(root, "result", rpc_res->result);
          }
          free(escaped_result);
        } else {
          cJSON_AddStringToObject(root, "result", rpc_res->result);
        }
      }
    } else {
      cJSON_AddNullToObject(root, "result");
    }
  }

  /* Add id */
  if (rpc_res->id) {
    cJSON *id = cJSON_Parse(rpc_res->id);
    if (id) {
      cJSON_AddItemToObject(root, "id", id);
    } else {
      cJSON_AddStringToObject(root, "id", rpc_res->id);
    }
  } else {
    cJSON_AddNullToObject(root, "id");
  }

  char *json_str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (!json_str)
    return -1;

  *output = turbo_arena_strdup(arena, json_str);
  *output_len = strlen(json_str);
  cJSON_free(json_str);

  return 0;
}

void rpc_send_response(Res *res, rpc_response_t *rpc_res) {
  if (!res || !rpc_res)
    return;

  char *output = NULL;
  size_t output_len = 0;

  if (rpc_build_response(rpc_res, &output, &output_len) == 0 && output) {
    send_json(res, 200, output);
  } else {
    send_json(res, 500,
              "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Internal "
              "error\"},\"id\":null}");
  }
}

void rpc_send_stream_start(Res *res, rpc_response_t *rpc_res) {
    (void)rpc_res;
    reply_stream_start(res, 200);
}

void rpc_send_stream_chunk(Res *res, rpc_response_t *rpc_res) {
    if (!res || !rpc_res) return;
    char *output = NULL;
    size_t output_len = 0;
    if (rpc_build_response(rpc_res, &output, &output_len) == 0 && output) {
        reply_stream_chunk(res, output);
    }
}

void rpc_send_stream_end(Res *res) {
    reply_stream_end(res);
}

void rpc_send_error(Res *res, int error_code, const char *error_message, const char *id) {
  if (!res)
    return;

  cJSON *root = cJSON_CreateObject();
  if (!root) {
    send_json(res, 500,
              "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Internal "
              "error\"},\"id\":null}");
    return;
  }

  cJSON_AddStringToObject(root, "jsonrpc", "2.0");

  cJSON *error = cJSON_CreateObject();
  cJSON_AddNumberToObject(error, "code", error_code);
  
  // Escape error message to prevent injection
  if (error_message) {
    char *escaped_message = malloc(strlen(error_message) * 2 + 256);
    if (escaped_message) {
      iris_security_result_t escape_result = iris_escape_json(error_message, escaped_message, strlen(error_message) * 2 + 256);
      if (escape_result == IRIS_SECURITY_OK) {
        cJSON_AddStringToObject(error, "message", escaped_message);
      } else {
        // Fallback to original message if escaping fails
        TLOG_WARN("Failed to escape RPC error message: {:s}", iris_security_error_string(escape_result));
        cJSON_AddStringToObject(error, "message", error_message);
      }
      free(escaped_message);
    } else {
      cJSON_AddStringToObject(error, "message", error_message);
    }
  } else {
    cJSON_AddStringToObject(error, "message", "Unknown error");
  }
  
  cJSON_AddItemToObject(root, "error", error);

  if (id) {
    cJSON *id_obj = cJSON_Parse(id);
    if (id_obj) {
      cJSON_AddItemToObject(root, "id", id_obj);
    } else {
      cJSON_AddStringToObject(root, "id", id);
    }
  } else {
    cJSON_AddNullToObject(root, "id");
  }

  char *response = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (response) {
    send_json(res, 200, response);
    cJSON_free(response);
  } else {
    send_json(res, 500,
              "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Internal "
              "error\"},\"id\":null}");
  }
}

void rpc_set_result(rpc_response_t *rpc_res, const char *result) {
  if (!rpc_res || !rpc_res->arena)
    return;

  rpc_res->result = turbo_arena_strdup(rpc_res->arena, result);
  rpc_res->error_code = 0;
  rpc_res->error_message = NULL;
}

void rpc_set_error(rpc_response_t *rpc_res, int error_code, const char *error_message) {
  if (!rpc_res || !rpc_res->arena)
    return;

  rpc_res->error_code = error_code;
  rpc_res->error_message = turbo_arena_strdup(rpc_res->arena, error_message);
  rpc_res->result = NULL;
}

/* No external declarations needed - post macro is in iris.h */

/* Get or create cached params object */
static cJSON *rpc_get_params_object(rpc_request_t *rpc_req) {
  if (!rpc_req || !rpc_req->params)
    return NULL;

  /* Parse params */
  cJSON *params = cJSON_Parse(rpc_req->params);
  return params;
}

const char *rpc_get_param_string(rpc_request_t *rpc_req, const char *key) {
  if (!rpc_req || !key)
    return NULL;

  cJSON *params = rpc_get_params_object(rpc_req);
  if (!params)
    return NULL;

  cJSON *item = cJSON_GetObjectItem(params, key);
  const char *result = NULL;

  if (item && cJSON_IsString(item)) {
    result = turbo_arena_strdup(rpc_req->arena, item->valuestring);
  }

  cJSON_Delete(params);
  return result;
}

int rpc_get_param_int(rpc_request_t *rpc_req, const char *key, int64_t *value) {
  if (!rpc_req || !key || !value)
    return -1;

  cJSON *params = rpc_get_params_object(rpc_req);
  if (!params)
    return -1;

  cJSON *item = cJSON_GetObjectItem(params, key);
  int result = -1;

  if (item && cJSON_IsNumber(item)) {
    *value = (int64_t)item->valuedouble;
    result = 0;
  }

  cJSON_Delete(params);
  return result;
}

int rpc_get_param_bool(rpc_request_t *rpc_req, const char *key, int *value) {
  if (!rpc_req || !key || !value)
    return -1;

  cJSON *params = rpc_get_params_object(rpc_req);
  if (!params)
    return -1;

  cJSON *item = cJSON_GetObjectItem(params, key);
  int result = -1;

  if (item && cJSON_IsBool(item)) {
    *value = cJSON_IsTrue(item) ? 1 : 0;
    result = 0;
  }

  cJSON_Delete(params);
  return result;
}

/* RPC endpoint handler */
static void rpc_endpoint_handler(Req *req, Res *res) {
  /* Get RPC context from global (set by rpc_setup_endpoint) */
  rpc_context_t *ctx = g_rpc_context;
  if (!ctx) {
    rpc_send_error(res, RPC_ERROR_INTERNAL, "RPC context not initialized", NULL);
    return;
  }

  /* Check request size */
  if (req->body_len > ctx->config.max_request_size) {
    rpc_send_error(res, RPC_ERROR_INVALID_REQUEST, "Request too large", NULL);
    return;
  }

  /* Parse RPC request */
  rpc_request_t rpc_req;
  int parse_result = rpc_parse_request(req, &rpc_req);
  if (parse_result != 0) {
    rpc_send_error(res, parse_result, "Failed to parse request", NULL);
    return;
  }

  /* Find method */
  rpc_method_handler_t handler = NULL;
  int requires_auth = 0;

  for (size_t i = 0; i < ctx->method_count; i++) {
    if (strcmp(ctx->methods[i].name, rpc_req.method) == 0) {
      handler = ctx->methods[i].handler;
      requires_auth = ctx->methods[i].requires_auth;
      break;
    }
  }

  if (!handler) {
    rpc_send_error(res, RPC_ERROR_METHOD_NOT_FOUND, "Method not found", rpc_req.id);
    return;
  }

  /* TODO: Check authentication if required */
  if (requires_auth) {
    /* Implement authentication check here */
  }

  /* Create RPC response */
  rpc_response_t rpc_res;
  memset(&rpc_res, 0, sizeof(rpc_res));
  rpc_res.arena = req->arena;
  rpc_res.jsonrpc = "2.0";
  rpc_res.id = rpc_req.id;
  rpc_res.protocol = rpc_req.protocol;

  /* Call handler */
  int result = handler(req, res, &rpc_req, &rpc_res);
  if (result == RPC_STREAMING) {
      /* Handler handled its own response (streaming) */
      return;
  }
  
  if (result != 0 && rpc_res.error_code == 0) {
    /* Handler returned error but didn't set error details */
    rpc_set_error(&rpc_res, RPC_ERROR_INTERNAL, "Method execution failed");
  }

  /* Send response */
  rpc_send_response(res, &rpc_res);
}

/* Introspection handler */
static int rpc_introspection_handler(Req *req, Res *res, rpc_request_t *rpc_req, rpc_response_t *rpc_res) {
  (void)req;
  (void)res;
  (void)rpc_req;

  rpc_context_t *ctx = g_rpc_context;
  if (!ctx) {
    rpc_set_error(rpc_res, RPC_ERROR_INTERNAL, "RPC context not initialized");
    return -1;
  }

  /* Build method list using cJSON */
  cJSON *methods_array = cJSON_CreateArray();
  if (!methods_array) {
    rpc_set_error(rpc_res, RPC_ERROR_INTERNAL, "Failed to create response");
    return -1;
  }

  for (size_t i = 0; i < ctx->method_count; i++) {
    cJSON *method_obj = cJSON_CreateObject();
    if (!method_obj) {
      cJSON_Delete(methods_array);
      rpc_set_error(rpc_res, RPC_ERROR_INTERNAL, "Failed to create response");
      return -1;
    }

    cJSON_AddStringToObject(method_obj, "name", ctx->methods[i].name);
    cJSON_AddStringToObject(method_obj, "description",
                            ctx->methods[i].description ? ctx->methods[i].description : "");
    cJSON_AddBoolToObject(method_obj, "requires_auth", ctx->methods[i].requires_auth);

    cJSON_AddItemToArray(methods_array, method_obj);
  }

  char *result = cJSON_PrintUnformatted(methods_array);
  cJSON_Delete(methods_array);

  if (!result) {
    rpc_set_error(rpc_res, RPC_ERROR_INTERNAL, "Failed to serialize response");
    return -1;
  }

  rpc_set_result(rpc_res, result);
  cJSON_free(result);
  return 0;
}

int rpc_setup_endpoint(rpc_context_t *ctx) {
  if (!ctx)
    return -1;

  /* Store context globally for handler access */
  g_rpc_context = ctx;

  /* Register introspection method if enabled */
  if (ctx->config.enable_introspection) {
    rpc_method_t introspection;
    introspection.name = "rpc.listMethods";
    introspection.handler = rpc_introspection_handler;
    introspection.description = "List all available RPC methods";
    introspection.requires_auth = 0;
    rpc_register_method(ctx, &introspection);
  }

  /* Register HTTP endpoint */
  post(ctx->config.endpoint, rpc_endpoint_handler);

  return 0;
}
