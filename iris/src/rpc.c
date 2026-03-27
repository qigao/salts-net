#include "rpc.h"
#include "iris.h"
#include "security.h"
#include <fmt.h>
#include <json_parser.h>
#include <stdlib.h>
#include <string.h>
#include "tlog.h"

/* Global RPC context - temporary solution until iris_app_t refactor */
static rpc_context_t *g_rpc_context = NULL;

/* Cached params object to avoid re-parsing */
typedef struct {
  json_value_t *params_obj;
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
  json_value_t *root = json_parse(req->body, req->body_len);
  if (!root)
    return RPC_ERROR_PARSE;

  /* Extract jsonrpc version */
  json_value_t *jsonrpc = json_object_get(root, "jsonrpc");
  if (jsonrpc && json_type(jsonrpc) == JSON_STRING) {
    rpc_req->jsonrpc = mem_strdup(req->arena, json_string(jsonrpc));
    if (!rpc_req->jsonrpc) {
      json_free(root);
      return RPC_ERROR_INTERNAL;
    }
  }

  /* Extract method (required) */
  json_value_t *method = json_object_get(root, "method");
  if (!method || json_type(method) != JSON_STRING) {
    json_free(root);
    return RPC_ERROR_INVALID_REQUEST;
  }
  rpc_req->method = mem_strdup(req->arena, json_string(method));
  if (!rpc_req->method) {
    json_free(root);
    return RPC_ERROR_INTERNAL;
  }

  /* Extract params (optional) */
  json_value_t *params = json_object_get(root, "params");
  if (params) {
    size_t params_len = 0;
    char *params_str = json_serialize(params, &params_len);
    if (params_str) {
      rpc_req->params = mem_strdup(req->arena, params_str);
      json_serialize_free(params_str);
      if (!rpc_req->params) {
        json_free(root);
        return RPC_ERROR_INTERNAL;
      }
    } else {
      json_free(root);
      return RPC_ERROR_INTERNAL;
    }
  }

  /* Extract id (optional for notifications) */
  json_value_t *id = json_object_get(root, "id");
  if (id) {
    if (json_type(id) != JSON_STRING && json_type(id) != JSON_NUMBER && !json_is_null(id)) {
      json_free(root);
      return RPC_ERROR_INVALID_REQUEST;
    }

    size_t id_len = 0;
    char *id_str = json_serialize(id, &id_len);
    if (!id_str) {
      json_free(root);
      return RPC_ERROR_INTERNAL;
    }

    rpc_req->id = mem_strdup(req->arena, id_str);
    json_serialize_free(id_str);
    if (!rpc_req->id) {
      json_free(root);
      return RPC_ERROR_INTERNAL;
    }
  }

  json_free(root);
  return 0;
}

int rpc_build_response(rpc_response_t *rpc_res, char **output, size_t *output_len) {
  if (!rpc_res || !output || !output_len)
    return -1;

  *output = NULL;
  *output_len = 0;

  /* Phase IRIS-1: Updated to use mem_pool_t */
  mem_pool_t *arena = rpc_res->arena;
  if (!arena)
    return -1;

  /* Build JSON-RPC response using json_parser */
  json_value_t *root = json_create_object();
  if (!root)
    return -1;

  json_object_set_string(root, "jsonrpc", "2.0");

  if (rpc_res->error_code != 0) {
    /* Error response */
    json_value_t *error = json_create_object();
    if (!error) {
      json_free(root);
      return -1;
    }
    json_object_set_number(error, "code", rpc_res->error_code);

    if (rpc_res->error_message) {
      json_object_set_string(error, "message", rpc_res->error_message);
    } else {
      json_object_set_string(error, "message", "Unknown error");
    }

    json_object_add(root, "error", error);
  } else {
    /* Success response */
    if (rpc_res->result) {
      json_value_t *result = json_parse(rpc_res->result, strlen(rpc_res->result));
      if (result) {
        json_object_add(root, "result", result);
      } else {
        json_free(root);
        return -1;
      }
    } else {
      json_object_set_null(root, "result");
    }
  }

  /* Add id */
  if (rpc_res->id) {
    json_value_t *id = json_parse(rpc_res->id, strlen(rpc_res->id));
    if (id) {
      json_object_add(root, "id", id);
    } else {
      json_free(root);
      return -1;
    }
  } else {
    json_object_set_null(root, "id");
  }

  size_t json_len = 0;
  char *json_str = json_serialize(root, &json_len);
  json_free(root);

  if (!json_str)
    return -1;

  *output = mem_strdup(arena, json_str);
  if (!*output) {
    json_serialize_free(json_str);
    return -1;
  }
  *output_len = json_len;
  json_serialize_free(json_str);

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
    } else {
        TLOG_ERROR("Failed to build RPC stream chunk");
        reply_stream_end(res);
    }
}

void rpc_send_stream_end(Res *res) {
    reply_stream_end(res);
}

void rpc_send_error(Res *res, int error_code, const char *error_message, const char *id) {
  if (!res)
    return;

  json_value_t *root = json_create_object();
  if (!root) {
    send_json(res, 500,
              "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Internal "
              "error\"},\"id\":null}");
    return;
  }

  json_object_set_string(root, "jsonrpc", "2.0");

  json_value_t *error = json_create_object();
  if (!error) {
    json_free(root);
    send_json(res, 500,
              "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Internal "
              "error\"},\"id\":null}");
    return;
  }
  json_object_set_number(error, "code", error_code);

  if (error_message) {
    json_object_set_string(error, "message", error_message);
  } else {
    json_object_set_string(error, "message", "Unknown error");
  }

  json_object_add(root, "error", error);

  if (id) {
    json_value_t *id_obj = json_parse(id, strlen(id));
    if (id_obj) {
      json_object_add(root, "id", id_obj);
    } else {
      json_free(root);
      send_json(res, 500,
                "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Internal "
                "error\"},\"id\":null}");
      return;
    }
  } else {
    json_object_set_null(root, "id");
  }

  size_t response_len = 0;
  char *response = json_serialize(root, &response_len);
  json_free(root);

  if (response) {
    send_json(res, 200, response);
    json_serialize_free(response);
  } else {
    send_json(res, 500,
              "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Internal "
              "error\"},\"id\":null}");
  }
}

void rpc_set_result(rpc_response_t *rpc_res, const char *result) {
  if (!rpc_res || !rpc_res->arena)
    return;

  rpc_res->result = mem_strdup(rpc_res->arena, result);
  if (!rpc_res->result) {
    rpc_res->error_code = RPC_ERROR_INTERNAL;
    rpc_res->error_message = "Out of memory";
    return;
  }
  rpc_res->error_code = 0;
  rpc_res->error_message = NULL;
}

void rpc_set_error(rpc_response_t *rpc_res, int error_code, const char *error_message) {
  if (!rpc_res || !rpc_res->arena)
    return;

  rpc_res->error_code = error_code;
  if (error_message) {
    rpc_res->error_message = mem_strdup(rpc_res->arena, error_message);
    if (!rpc_res->error_message) {
      rpc_res->error_code = RPC_ERROR_INTERNAL;
      rpc_res->error_message = "Out of memory";
    }
  } else {
    rpc_res->error_message = "Unknown error";
  }
  rpc_res->result = NULL;
}

/* No external declarations needed - post macro is in iris.h */

/* Get or create cached params object */
static json_value_t *rpc_get_params_object(rpc_request_t *rpc_req) {
  if (!rpc_req || !rpc_req->params)
    return NULL;

  /* Parse params */
  json_value_t *params = json_parse(rpc_req->params, strlen(rpc_req->params));
  return params;
}

const char *rpc_get_param_string(rpc_request_t *rpc_req, const char *key) {
  if (!rpc_req || !key)
    return NULL;

  json_value_t *params = rpc_get_params_object(rpc_req);
  if (!params)
    return NULL;

  json_value_t *item = json_object_get(params, key);
  const char *result = NULL;

  if (item && json_type(item) == JSON_STRING) {
    result = mem_strdup(rpc_req->arena, json_string(item));
  }

  json_free(params);
  return result;
}

int rpc_get_param_int(rpc_request_t *rpc_req, const char *key, int64_t *value) {
  if (!rpc_req || !key || !value)
    return -1;

  json_value_t *params = rpc_get_params_object(rpc_req);
  if (!params)
    return -1;

  json_value_t *item = json_object_get(params, key);
  int result = -1;

  if (item && json_type(item) == JSON_NUMBER) {
    *value = (int64_t)json_number(item);
    result = 0;
  }

  json_free(params);
  return result;
}

int rpc_get_param_bool(rpc_request_t *rpc_req, const char *key, int *value) {
  if (!rpc_req || !key || !value)
    return -1;

  json_value_t *params = rpc_get_params_object(rpc_req);
  if (!params)
    return -1;

  json_value_t *item = json_object_get(params, key);
  int result = -1;

  if (item && json_type(item) == JSON_BOOL) {
    *value = json_bool(item) ? 1 : 0;
    result = 0;
  }

  json_free(params);
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

  /* Build method list using json_parser */
  json_value_t *methods_array = json_create_array();
  if (!methods_array) {
    rpc_set_error(rpc_res, RPC_ERROR_INTERNAL, "Failed to create response");
    return -1;
  }

  for (size_t i = 0; i < ctx->method_count; i++) {
    json_value_t *method_obj = json_create_object();
    if (!method_obj) {
      json_free(methods_array);
      rpc_set_error(rpc_res, RPC_ERROR_INTERNAL, "Failed to create response");
      return -1;
    }

    json_object_set_string(method_obj, "name", ctx->methods[i].name);
    json_object_set_string(method_obj, "description",
                            ctx->methods[i].description ? ctx->methods[i].description : "");
    json_object_set_bool(method_obj, "requires_auth", ctx->methods[i].requires_auth);

    json_array_add(methods_array, method_obj);
  }

  size_t result_len = 0;
  char *result = json_serialize(methods_array, &result_len);
  json_free(methods_array);

  if (!result) {
    rpc_set_error(rpc_res, RPC_ERROR_INTERNAL, "Failed to serialize response");
    return -1;
  }

  rpc_set_result(rpc_res, result);
  json_serialize_free(result);
  return 0;
}

int rpc_setup_endpoint(rpc_context_t *ctx) {
  iris_app_t *app;

  if (!ctx)
    return -1;

  /* Store context globally for handler access */
  g_rpc_context = ctx;
  app = iris_app_default();
  if (app) {
    app->rpc_context = ctx;
  }

  /* Register introspection method if enabled */
  if (ctx->config.enable_introspection) {
    rpc_method_t introspection = {0};
    introspection.name = "rpc.listMethods";
    introspection.handler = rpc_introspection_handler;
    introspection.description = "List all available RPC methods";
    introspection.requires_auth = 0;
    if (rpc_register_method(ctx, &introspection) != 0) {
      return -1;
    }
  }

  /* Register HTTP endpoint */
  post(ctx->config.endpoint, rpc_endpoint_handler);

  return 0;
}
