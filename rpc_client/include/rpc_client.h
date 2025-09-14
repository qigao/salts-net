#ifndef RPC_CLIENT_H
#define RPC_CLIENT_H

#include "rpc_error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file rpc_client.h
 * @brief Lightweight JSON-RPC 2.0 client library
 * 
 * A minimal RPC client with no external dependencies (no llhttp, no JSON library).
 * Uses simple string operations for HTTP and JSON handling.
 */

/* Forward declarations */
typedef struct rpc_client_s rpc_client_t;

/**
 * @brief Transport types
 */
typedef enum {
  RPC_TRANSPORT_TCP = 0,
  RPC_TRANSPORT_UDP,
  RPC_TRANSPORT_TLS,
  RPC_TRANSPORT_UNIX
} rpc_transport_t;

/**
 * @brief Client configuration
 */
typedef struct {
  const char *host;              /**< Server hostname or IP */
  int port;                      /**< Server port */
  const char *endpoint;          /**< RPC endpoint path (e.g., "/rpc") */
  rpc_transport_t transport;     /**< Transport type */
  int timeout_ms;                /**< Request timeout in milliseconds */
  int keep_alive;                /**< Keep connection alive */
  const char *user_agent;        /**< User-Agent header */
} rpc_client_config_t;

/**
 * @brief Call result
 */
typedef struct {
  int success;                   /**< 1 if successful, 0 if error */
  char *result;                  /**< Result JSON string (if success) */
  int error_code;                /**< Error code (if error) */
  char *error_message;           /**< Error message (if error) */
  char *id;                      /**< Request ID */
  int http_status;               /**< HTTP status code */
} rpc_call_result_t;

/**
 * @brief Async callback
 */
typedef void (*rpc_callback_t)(rpc_call_result_t *result, void *user_data);

/**
 * @brief Connection state
 */
typedef enum {
  RPC_STATE_DISCONNECTED = 0,
  RPC_STATE_CONNECTING,
  RPC_STATE_CONNECTED,
  RPC_STATE_ERROR
} rpc_client_state_t;

/* ============================================================================
 * Client Lifecycle
 * ============================================================================ */

/**
 * @brief Create RPC client
 * 
 * @param config Client configuration
 * @return Client instance or NULL on failure
 */
rpc_client_t *rpc_client_create(const rpc_client_config_t *config);

/**
 * @brief Destroy RPC client
 * 
 * @param client Client instance
 */
void rpc_client_destroy(rpc_client_t *client);

/**
 * @brief Connect to server
 * 
 * @param client Client instance
 * @return 0 on success, -1 on failure
 */
int rpc_client_connect(rpc_client_t *client);

/**
 * @brief Disconnect from server
 * 
 * @param client Client instance
 */
void rpc_client_disconnect(rpc_client_t *client);

/**
 * @brief Get connection state
 * 
 * @param client Client instance
 * @return Connection state
 */
rpc_client_state_t rpc_client_get_state(rpc_client_t *client);

/* ============================================================================
 * RPC Calls
 * ============================================================================ */

/**
 * @brief Make synchronous RPC call
 * 
 * @param client Client instance
 * @param method Method name
 * @param params Parameters (JSON string, can be NULL)
 * @param result Result (output, must be freed with rpc_result_free)
 * @return 0 on success, -1 on failure
 * 
 * @example
 * rpc_call_result_t result;
 * rpc_client_call(client, "math.add", "{\"a\":5,\"b\":3}", &result);
 * if (result.success) {
 *   printf("Result: %s\n", result.result);
 * }
 * rpc_result_free(&result);
 */
int rpc_client_call(rpc_client_t *client, const char *method, const char *params,
                    rpc_call_result_t *result);

/**
 * @brief Make asynchronous RPC call
 * 
 * @param client Client instance
 * @param method Method name
 * @param params Parameters (JSON string, can be NULL)
 * @param callback Callback function
 * @param user_data User data for callback
 * @return 0 on success, -1 on failure
 */
int rpc_client_call_async(rpc_client_t *client, const char *method, const char *params,
                          rpc_callback_t callback, void *user_data);

/**
 * @brief Send notification (no response expected)
 * 
 * @param client Client instance
 * @param method Method name
 * @param params Parameters (JSON string, can be NULL)
 * @return 0 on success, -1 on failure
 */
int rpc_client_notify(rpc_client_t *client, const char *method, const char *params);

/**
 * @brief Make batch RPC call
 * 
 * @param client Client instance
 * @param methods Array of method names
 * @param params Array of parameter strings
 * @param count Number of calls
 * @param results Array of results (output, must be freed)
 * @return 0 on success, -1 on failure
 */
int rpc_client_batch_call(rpc_client_t *client, const char **methods, const char **params,
                          size_t count, rpc_call_result_t *results);

/* ============================================================================
 * Result Handling
 * ============================================================================ */

/**
 * @brief Free call result
 * 
 * @param result Call result
 */
void rpc_result_free(rpc_call_result_t *result);

/**
 * @brief Check if result is successful
 * 
 * @param result Call result
 * @return 1 if successful, 0 otherwise
 */
int rpc_result_is_success(const rpc_call_result_t *result);

/**
 * @brief Get error string
 * 
 * @param result Call result
 * @return Error string or NULL
 */
const char *rpc_result_get_error(const rpc_call_result_t *result);

/* ============================================================================
 * Parameter Building Helpers
 * ============================================================================ */

/**
 * @brief Build params from format string
 * 
 * @param format Format string (e.g., "{\"a\":%d,\"b\":%d}")
 * @param ... Format arguments
 * @return Params string (must be freed)
 * 
 * @example
 * char *params = rpc_build_params("{\"a\":%d,\"b\":%d}", 5, 3);
 * rpc_client_call(client, "math.add", params, &result);
 * free(params);
 */
char *rpc_build_params(const char *format, ...);

/**
 * @brief Build params with named arguments
 * 
 * @param ... Key-value pairs (const char *key, value, ..., NULL)
 * @return Params string (must be freed)
 * 
 * @example
 * char *params = rpc_build_params_named(
 *   "name", "John",
 *   "age", 30,
 *   NULL
 * );
 */
char *rpc_build_params_named(const char *key, ...);

/* ============================================================================
 * Result Extraction Helpers
 * ============================================================================ */

/**
 * @brief Extract string from result
 * 
 * @param result Call result
 * @param key Key name (NULL for root value)
 * @return Value or NULL (must be freed)
 */
char *rpc_result_get_string(const rpc_call_result_t *result, const char *key);

/**
 * @brief Extract integer from result
 * 
 * @param result Call result
 * @param key Key name (NULL for root value)
 * @param value Output value
 * @return 0 on success, -1 on failure
 */
int rpc_result_get_int(const rpc_call_result_t *result, const char *key, int64_t *value);

/**
 * @brief Extract boolean from result
 * 
 * @param result Call result
 * @param key Key name (NULL for root value)
 * @param value Output value
 * @return 0 on success, -1 on failure
 */
int rpc_result_get_bool(const rpc_call_result_t *result, const char *key, int *value);

/**
 * @brief Extract double from result
 * 
 * @param result Call result
 * @param key Key name (NULL for root value)
 * @param value Output value
 * @return 0 on success, -1 on failure
 */
int rpc_result_get_double(const rpc_call_result_t *result, const char *key, double *value);

/* ============================================================================
 * Configuration Helpers
 * ============================================================================ */

/**
 * @brief Default configuration
 */
#define RPC_CLIENT_DEFAULT_CONFIG(host, port) \
  { \
    (const char *)(host), \
    (port), \
    "/rpc", \
    RPC_TRANSPORT_TCP, \
    5000, \
    1, \
    "rpc_client/1.0" \
  }

/**
 * @brief TLS configuration
 */
#define RPC_CLIENT_TLS_CONFIG(host, port) \
  { \
    (const char *)(host), \
    (port), \
    "/rpc", \
    RPC_TRANSPORT_TLS, \
    5000, \
    1, \
    "rpc_client/1.0" \
  }

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * @brief Get library version
 *
 * @return Version string
 */
const char *rpc_client_version(void);

#ifdef __cplusplus
}
#endif

#endif /* RPC_CLIENT_H */
