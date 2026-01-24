#ifndef IRIS_RPC_H
#define IRIS_RPC_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct Req Req;
typedef struct Res Res;

#include "router.h"
/* Phase IRIS-1: Use turbo_arena instead of vendor arena */
#include "arena_buffer.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RPC Protocol Types
 */
typedef enum {
  RPC_PROTOCOL_JSON = 0, /**< JSON-RPC 2.0 */
  RPC_PROTOCOL_MSGPACK,  /**< MessagePack-RPC */
  RPC_PROTOCOL_CBOR,     /**< CBOR-RPC */
  RPC_PROTOCOL_PROTOBUF  /**< Protocol Buffers */
} rpc_protocol_t;

/**
 * @brief RPC Error Codes (JSON-RPC 2.0 compatible)
 */
typedef enum {
  RPC_ERROR_PARSE = -32700,            /**< Parse error */
  RPC_ERROR_INVALID_REQUEST = -32600,  /**< Invalid Request */
  RPC_ERROR_METHOD_NOT_FOUND = -32601, /**< Method not found */
  RPC_ERROR_INVALID_PARAMS = -32602,   /**< Invalid params */
  RPC_ERROR_INTERNAL = -32603,         /**< Internal error */
  RPC_ERROR_SERVER_MIN = -32099,       /**< Server error start */
  RPC_ERROR_SERVER_MAX = -32000,       /**< Server error end */
  RPC_STREAMING = 1                    /**< Special return code to indicate streaming */
} rpc_error_code_t;

/**
 * @brief RPC Request structure
 */
typedef struct {
  turbo_arena_t *arena;    /**< Arena for memory allocation */
  char *jsonrpc;           /**< Protocol version (e.g., "2.0") */
  char *method;            /**< Method name */
  char *params;            /**< Parameters (JSON string) */
  char *id;                /**< Request ID (can be NULL for notifications) */
  rpc_protocol_t protocol; /**< Protocol type */
  void *raw_params;        /**< Raw parameter data (for binary protocols) */
  size_t raw_params_len;   /**< Length of raw parameters */
} rpc_request_t;

/**
 * @brief RPC Response structure
 */
typedef struct {
  turbo_arena_t *arena;    /**< Arena for memory allocation */
  char *jsonrpc;           /**< Protocol version */
  char *result;            /**< Result (JSON string) */
  char *error_message;     /**< Error message */
  int error_code;          /**< Error code */
  char *id;                /**< Request ID */
  rpc_protocol_t protocol; /**< Protocol type */
  void *raw_result;        /**< Raw result data (for binary protocols) */
  size_t raw_result_len;   /**< Length of raw result */
} rpc_response_t;

/**
 * @brief RPC Method Handler
 *
 * @param req HTTP request
 * @param rpc_req RPC request
 * @param rpc_res RPC response
 * @return 0 on success, error code otherwise
 */
typedef int (*rpc_method_handler_t)(Req *req, Res *res, rpc_request_t *rpc_req,
                                    rpc_response_t *rpc_res);

/**
 * @brief RPC Method Registration
 */
typedef struct {
  const char *name;             /**< Method name */
  rpc_method_handler_t handler; /**< Handler function */
  const char *description;      /**< Method description */
  int requires_auth;            /**< Requires authentication */
} rpc_method_t;

/**
 * @brief RPC Server Configuration
 */
typedef struct {
  const char *endpoint;            /**< RPC endpoint path (e.g., "/rpc") */
  rpc_protocol_t default_protocol; /**< Default protocol */
  int enable_introspection;        /**< Enable method introspection */
  int enable_batch;                /**< Enable batch requests */
  int max_batch_size;              /**< Maximum batch size */
  size_t max_request_size;         /**< Maximum request size */
} rpc_config_t;

/**
 * @brief RPC Context
 */
typedef struct rpc_context_s {
  rpc_config_t config;
  rpc_method_t *methods;
  size_t method_count;
  size_t method_capacity;
  turbo_arena_t *arena;
} rpc_context_t;

/**
 * @brief Initialize RPC context
 *
 * @param config RPC configuration
 * @return RPC context or NULL on failure
 */
rpc_context_t *rpc_init(const rpc_config_t *config);

/**
 * @brief Destroy RPC context
 *
 * @param ctx RPC context
 */
void rpc_destroy(rpc_context_t *ctx);

/**
 * @brief Register an RPC method
 *
 * @param ctx RPC context
 * @param method Method registration info
 * @return 0 on success, -1 on failure
 */
int rpc_register_method(rpc_context_t *ctx, const rpc_method_t *method);

/**
 * @brief Unregister an RPC method
 *
 * @param ctx RPC context
 * @param method_name Method name
 * @return 0 on success, -1 on failure
 */
int rpc_unregister_method(rpc_context_t *ctx, const char *method_name);

/**
 * @brief Setup RPC endpoint in Iris router
 *
 * @param ctx RPC context
 * @return 0 on success, -1 on failure
 */
int rpc_setup_endpoint(rpc_context_t *ctx);

/**
 * @brief Parse RPC request from HTTP request
 *
 * @param req HTTP request
 * @param rpc_req RPC request (output)
 * @return 0 on success, error code otherwise
 */
int rpc_parse_request(Req *req, rpc_request_t *rpc_req);

/**
 * @brief Build RPC response
 *
 * @param rpc_res RPC response
 * @param output Output buffer (output)
 * @param output_len Output length (output)
 * @return 0 on success, -1 on failure
 */
int rpc_build_response(rpc_response_t *rpc_res, char **output, size_t *output_len);

/**
 * @brief Send RPC response
 *
 * @param res HTTP response
 * @param rpc_res RPC response
 */
void rpc_send_response(Res *res, rpc_response_t *rpc_res);

/**
 * @brief Start an RPC stream (SSE)
 * 
 * @param res HTTP response
 * @param rpc_res RPC response (metadata like id will be used)
 */
void rpc_send_stream_start(Res *res, rpc_response_t *rpc_res);

/**
 * @brief Send an RPC stream chunk (SSE event)
 * 
 * @param res HTTP response
 * @param rpc_res RPC response (result will be sent as SSE data)
 */
void rpc_send_stream_chunk(Res *res, rpc_response_t *rpc_res);

/**
 * @brief End an RPC stream (SSE)
 * 
 * @param res HTTP response
 */
void rpc_send_stream_end(Res *res);

/**
 * @brief Send RPC error
 *
 * @param res HTTP response
 * @param error_code Error code
 * @param error_message Error message
 * @param id Request ID (can be NULL)
 */
void rpc_send_error(Res *res, int error_code, const char *error_message, const char *id);

/**
 * @brief Helper: Set RPC result (JSON string)
 *
 * @param rpc_res RPC response
 * @param result Result JSON string
 */
void rpc_set_result(rpc_response_t *rpc_res, const char *result);

/**
 * @brief Helper: Set RPC error
 *
 * @param rpc_res RPC response
 * @param error_code Error code
 * @param error_message Error message
 */
void rpc_set_error(rpc_response_t *rpc_res, int error_code, const char *error_message);

/**
 * @brief Helper: Get string parameter
 *
 * @param rpc_req RPC request
 * @param key Parameter key
 * @return Parameter value or NULL
 */
const char *rpc_get_param_string(rpc_request_t *rpc_req, const char *key);

/**
 * @brief Helper: Get integer parameter
 *
 * @param rpc_req RPC request
 * @param key Parameter key
 * @param value Output value
 * @return 0 on success, -1 on failure
 */
int rpc_get_param_int(rpc_request_t *rpc_req, const char *key, int64_t *value);

/**
 * @brief Helper: Get boolean parameter
 *
 * @param rpc_req RPC request
 * @param key Parameter key
 * @param value Output value
 * @return 0 on success, -1 on failure
 */
int rpc_get_param_bool(rpc_request_t *rpc_req, const char *key, int *value);

/**
 * @brief Convenience macro for defining RPC methods
 */
#define RPC_METHOD(name, handler, desc)                                                            \
  {.name = name, .handler = handler, .description = desc, .requires_auth = 0}

#define RPC_METHOD_AUTH(name, handler, desc)                                                       \
  {.name = name, .handler = handler, .description = desc, .requires_auth = 1}

/**
 * @brief Default RPC configuration
 */
#define RPC_DEFAULT_CONFIG()                                                                       \
  {.endpoint = "/rpc",                                                                             \
   .default_protocol = RPC_PROTOCOL_JSON,                                                          \
   .enable_introspection = 1,                                                                      \
   .enable_batch = 1,                                                                              \
   .max_batch_size = 10,                                                                           \
   .max_request_size = 1024 * 1024}

#ifdef __cplusplus
}
#endif

#endif /* IRIS_RPC_H */
