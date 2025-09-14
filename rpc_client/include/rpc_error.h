#ifndef RPC_ERROR_H
#define RPC_ERROR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file rpc_error.h
 * @brief RPC Client Error Handling Framework
 *
 * Provides detailed error codes, error tracking, and error recovery mechanisms
 * for production-grade RPC client operations.
 */

// ============================================================================
// Error Codes
// ============================================================================

/**
 * RPC Client error codes
 */
typedef enum {
  RPC_OK = 0,                           // Success

  // JSON-RPC 2.0 Standard Errors
  RPC_ERROR_PARSE = -32700,              // Invalid JSON
  RPC_ERROR_INVALID_REQUEST = -32600,    // Invalid Request object
  RPC_ERROR_METHOD_NOT_FOUND = -32601,   // Method not found
  RPC_ERROR_INVALID_PARAMS = -32602,     // Invalid method parameters
  RPC_ERROR_INTERNAL = -32603,           // Internal JSON-RPC error

  // Transport Errors (100-199)
  RPC_ERROR_CONNECT_FAILED = 100,        // Connection failed
  RPC_ERROR_CONNECTION_LOST = 101,       // Connection lost during request
  RPC_ERROR_TIMEOUT = 102,               // Request timeout
  RPC_ERROR_NETWORK = 103,               // Network I/O error
  RPC_ERROR_DNS = 104,                   // DNS resolution failed
  RPC_ERROR_TLS = 105,                   // TLS/SSL error

  // HTTP Errors (200-299)
  RPC_ERROR_HTTP_400 = 200,              // Bad Request
  RPC_ERROR_HTTP_401 = 201,              // Unauthorized
  RPC_ERROR_HTTP_403 = 202,              // Forbidden
  RPC_ERROR_HTTP_404 = 203,              // Not Found
  RPC_ERROR_HTTP_500 = 204,              // Internal Server Error
  RPC_ERROR_HTTP_502 = 205,              // Bad Gateway
  RPC_ERROR_HTTP_503 = 206,              // Service Unavailable
  RPC_ERROR_HTTP_OTHER = 299,            // Other HTTP error

  // Client Errors (300-399)
  RPC_ERROR_INVALID_CONFIG = 300,        // Invalid configuration
  RPC_ERROR_NOT_CONNECTED = 301,         // Client not connected
  RPC_ERROR_ALREADY_CONNECTED = 302,     // Already connected
  RPC_ERROR_INVALID_STATE = 303,         // Invalid client state
  RPC_ERROR_BUFFER_OVERFLOW = 304,       // Buffer overflow
  RPC_ERROR_OUT_OF_MEMORY = 305,         // Memory allocation failed
  RPC_ERROR_INVALID_RESPONSE = 306,      // Invalid RPC response format
  RPC_ERROR_POOL_EXHAUSTED = 307,        // Connection pool exhausted
  RPC_ERROR_CIRCUIT_OPEN = 308,          // Circuit breaker open

  // Retry Errors (400-499)
  RPC_ERROR_MAX_RETRIES = 400,           // Maximum retries exceeded
  RPC_ERROR_RETRY_TIMEOUT = 401,         // Retry timeout exceeded
  RPC_ERROR_BACKOFF_LIMIT = 402,         // Backoff limit reached

  // Resource Errors (500-599)
  RPC_ERROR_TOO_MANY_REQUESTS = 500,     // Rate limit exceeded
  RPC_ERROR_REQUEST_TOO_LARGE = 501,     // Request payload too large
  RPC_ERROR_RESPONSE_TOO_LARGE = 502,    // Response payload too large
  RPC_ERROR_QUEUE_FULL = 503,            // Request queue full

} rpc_error_code_t;

/**
 * Error severity levels
 */
typedef enum {
  RPC_SEVERITY_INFO = 0,      // Informational
  RPC_SEVERITY_WARNING = 1,   // Warning - recoverable
  RPC_SEVERITY_ERROR = 2,     // Error - may be recoverable
  RPC_SEVERITY_FATAL = 3      // Fatal - not recoverable
} rpc_error_severity_t;

/**
 * Error information structure
 */
typedef struct {
  rpc_error_code_t code;           // Error code
  rpc_error_severity_t severity;   // Severity level
  char message[256];               // Error message
  char details[512];               // Detailed error information
  const char *file;                // Source file (for debugging)
  int line;                        // Source line (for debugging)
  uint64_t timestamp;              // Error timestamp (microseconds)
  int http_status;                 // HTTP status code (if applicable)
  int retry_count;                 // Number of retries attempted
} rpc_error_info_t;

/**
 * Error callback for custom error handling
 */
typedef void (*rpc_error_callback_t)(const rpc_error_info_t *error, void *user_data);

// ============================================================================
// Error Handling Functions
// ============================================================================

/**
 * Get error name string
 *
 * @param code Error code
 * @return Error name (e.g., "RPC_ERROR_TIMEOUT")
 */
const char *rpc_error_name(rpc_error_code_t code);

/**
 * Get error description
 *
 * @param code Error code
 * @return Human-readable error description
 */
const char *rpc_error_description(rpc_error_code_t code);

/**
 * Get error severity
 *
 * @param code Error code
 * @return Error severity level
 */
rpc_error_severity_t rpc_error_get_severity(rpc_error_code_t code);

/**
 * Check if error is retryable
 *
 * @param code Error code
 * @return 1 if retryable, 0 otherwise
 */
int rpc_error_is_retryable(rpc_error_code_t code);

/**
 * Check if error is fatal
 *
 * @param code Error code
 * @return 1 if fatal, 0 otherwise
 */
int rpc_error_is_fatal(rpc_error_code_t code);

/**
 * Create error info structure
 *
 * @param code Error code
 * @param message Error message (optional)
 * @param details Detailed information (optional)
 * @return Error info structure
 */
rpc_error_info_t rpc_error_create(rpc_error_code_t code, const char *message,
                                   const char *details);

/**
 * Create error info with source location
 *
 * @param code Error code
 * @param message Error message
 * @param file Source file
 * @param line Source line
 * @return Error info structure
 */
rpc_error_info_t rpc_error_create_with_location(rpc_error_code_t code,
                                                  const char *message,
                                                  const char *file,
                                                  int line);

/**
 * Format error message
 *
 * @param error Error info
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Number of characters written
 */
size_t rpc_error_format(const rpc_error_info_t *error, char *buffer, size_t buffer_size);

/**
 * Get HTTP error code from HTTP status
 *
 * @param http_status HTTP status code
 * @return RPC error code
 */
rpc_error_code_t rpc_error_from_http_status(int http_status);

// ============================================================================
// Macros for Convenient Error Creation
// ============================================================================

/**
 * Create error with source location
 */
#define RPC_ERROR(code, msg) \
  rpc_error_create_with_location((code), (msg), __FILE__, __LINE__)

/**
 * Create error with formatted message
 */
#define RPC_ERROR_FMT(code, fmt, ...) \
  rpc_error_create_with_location((code), NULL, __FILE__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif // RPC_ERROR_H
