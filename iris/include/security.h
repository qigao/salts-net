#ifndef IRIS_SECURITY_H
#define IRIS_SECURITY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Security context structure for tracking request validation state
 * 
 * This structure maintains the security state for each request, including
 * validation status, rate limiting information, and security flags.
 */
typedef struct iris_security_context {
    // Request validation state
    bool headers_validated;
    bool body_validated;
    bool url_validated;
    bool cookies_validated;
    bool output_escaped;
    
    // Rate limiting state
    time_t last_request_time;
    int request_count;
    
    // Security flags
    bool suspicious_activity;
    int threat_level;
    int security_level;
} iris_security_context_t;

/**
 * @file security.h
 * @brief Security validation and sanitization functions for Iris framework
 * 
 * This module provides centralized input validation and output sanitization
 * to protect against common web vulnerabilities including injection attacks,
 * XSS, and other security threats.
 */

/* Security error codes */
typedef enum {
    IRIS_SECURITY_OK = 0,
    IRIS_SECURITY_ERROR_INVALID_INPUT = -1,
    IRIS_SECURITY_ERROR_SIZE_EXCEEDED = -2,
    IRIS_SECURITY_ERROR_MALICIOUS_CONTENT = -3,
    IRIS_SECURITY_ERROR_INVALID_FORMAT = -4,
    IRIS_SECURITY_ERROR_NULL_POINTER = -5,
    IRIS_SECURITY_ERROR_BUFFER_TOO_SMALL = -6
} iris_security_result_t;

/* Security limits - configurable at runtime */
typedef struct {
    size_t max_header_name_length;
    size_t max_header_value_length;
    size_t max_url_length;
    size_t max_cookie_name_length;
    size_t max_cookie_value_length;
    size_t max_json_depth;
    size_t max_log_message_length;
    size_t max_request_body_size;
    int max_headers_count;
} iris_security_limits_t;

/* Default security limits */
extern const iris_security_limits_t IRIS_DEFAULT_SECURITY_LIMITS;

/**
 * @brief Initialize security module with custom limits
 * @param limits Security limits configuration (NULL for defaults)
 * @return IRIS_SECURITY_OK on success, error code on failure
 */
CXX_C_API iris_security_result_t iris_security_init(const iris_security_limits_t *limits);

/**
 * @brief Get current security limits
 * @return Pointer to current security limits
 */
CXX_C_API const iris_security_limits_t *iris_security_get_limits(void);

/* ============================================================================
 * HTTP Header Validation
 * ============================================================================ */

/**
 * @brief Validate HTTP header field name according to RFC 7230
 * @param name Header field name to validate
 * @param max_len Maximum allowed length
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_http_header_name(const char *name, size_t max_len);

/**
 * @brief Validate HTTP header field value according to RFC 7230
 * @param value Header field value to validate
 * @param max_len Maximum allowed length
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_http_header_value(const char *value, size_t max_len);

/**
 * @brief Validate complete HTTP header (name and value)
 * @param name Header field name
 * @param value Header field value
 * @param max_name_len Maximum allowed name length
 * @param max_value_len Maximum allowed value length
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_http_header(const char *name, const char *value, 
                                                          size_t max_name_len, size_t max_value_len);

/* ============================================================================
 * URL and Parameter Validation
 * ============================================================================ */

/**
 * @brief Validate and sanitize URL path
 * @param path URL path to validate
 * @param max_len Maximum allowed length
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_url_path(const char *path, size_t max_len);

/**
 * @brief Sanitize URL parameter to prevent injection attacks
 * @param param Parameter value to sanitize
 * @param sanitized Buffer to store sanitized parameter
 * @param buffer_size Size of sanitized buffer
 * @return IRIS_SECURITY_OK on success, error code on failure
 */
CXX_C_API iris_security_result_t iris_sanitize_url_parameter(const char *param, 
                                                            char *sanitized, size_t buffer_size);

/**
 * @brief Check if URL parameter contains potentially malicious content
 * @param param Parameter to check
 * @return true if suspicious content detected, false otherwise
 */
CXX_C_API bool iris_is_suspicious_parameter(const char *param);

/* ============================================================================
 * Cookie Validation
 * ============================================================================ */

/**
 * @brief Validate cookie name according to RFC 6265
 * @param name Cookie name to validate
 * @param max_len Maximum allowed length
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_cookie_name(const char *name, size_t max_len);

/**
 * @brief Validate cookie value according to RFC 6265
 * @param value Cookie value to validate
 * @param max_len Maximum allowed length
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_cookie_value(const char *value, size_t max_len);

/**
 * @brief Validate complete cookie (name and value)
 * @param name Cookie name
 * @param value Cookie value
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_cookie(const char *name, const char *value);

/* ============================================================================
 * Output Sanitization and Escaping
 * ============================================================================ */

/**
 * @brief Escape HTML content to prevent XSS attacks
 * @param input Input string to escape
 * @param output Buffer to store escaped output
 * @param output_size Size of output buffer
 * @return IRIS_SECURITY_OK on success, error code on failure
 */
CXX_C_API iris_security_result_t iris_escape_html(const char *input, char *output, size_t output_size);

/**
 * @brief Escape JSON string to prevent injection
 * @param input Input string to escape
 * @param output Buffer to store escaped output
 * @param output_size Size of output buffer
 * @return IRIS_SECURITY_OK on success, error code on failure
 */
CXX_C_API iris_security_result_t iris_escape_json(const char *input, char *output, size_t output_size);

/**
 * @brief Escape JavaScript string to prevent injection
 * @param input Input string to escape
 * @param output Buffer to store escaped output
 * @param output_size Size of output buffer
 * @return IRIS_SECURITY_OK on success, error code on failure
 */
CXX_C_API iris_security_result_t iris_escape_javascript(const char *input, char *output, size_t output_size);

/**
 * @brief Escape URL component to prevent injection
 * @param input Input string to escape
 * @param output Buffer to store escaped output
 * @param output_size Size of output buffer
 * @return IRIS_SECURITY_OK on success, error code on failure
 */
CXX_C_API iris_security_result_t iris_escape_url(const char *input, char *output, size_t output_size);

/* ============================================================================
 * JSON-RPC Validation
 * ============================================================================ */

/**
 * @brief Validate JSON-RPC request structure
 * @param json JSON string to validate
 * @param max_len Maximum allowed length
 * @param max_depth Maximum allowed nesting depth
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_json_rpc(const char *json, size_t max_len, int max_depth);

/**
 * @brief Validate JSON structure and detect potential attacks
 * @param json JSON string to validate
 * @param max_len Maximum allowed length
 * @param max_depth Maximum allowed nesting depth
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_json_structure(const char *json, size_t max_len, int max_depth);

/* ============================================================================
 * Log Sanitization
 * ============================================================================ */

/**
 * @brief Sanitize log output to prevent log injection attacks
 * @param input Input string to sanitize
 * @param output Buffer to store sanitized output
 * @param output_size Size of output buffer
 * @return IRIS_SECURITY_OK on success, error code on failure
 */
CXX_C_API iris_security_result_t iris_sanitize_log_output(const char *input, char *output, size_t output_size);

/**
 * @brief Check if string contains log injection patterns
 * @param input String to check
 * @return true if log injection detected, false otherwise
 */
CXX_C_API bool iris_detect_log_injection(const char *input);

/* ============================================================================
 * Content Type and File Validation
 * ============================================================================ */

/**
 * @brief Validate content type header
 * @param content_type Content type string to validate
 * @param max_len Maximum allowed length
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_content_type(const char *content_type, size_t max_len);

/**
 * @brief Validate file extension for uploads
 * @param filename Filename to validate
 * @param allowed_extensions Array of allowed extensions (NULL-terminated)
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_file_extension(const char *filename, 
                                                             const char **allowed_extensions);

/**
 * @brief Check if file size is within limits
 * @param file_size Size of file in bytes
 * @param max_size Maximum allowed size
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_file_size(size_t file_size, size_t max_size);

/* ============================================================================
 * Security Utilities
 * ============================================================================ */

/**
 * @brief Generate secure random bytes
 * @param buffer Buffer to fill with random bytes
 * @param size Number of bytes to generate
 * @return IRIS_SECURITY_OK on success, error code on failure
 */
CXX_C_API iris_security_result_t iris_generate_random_bytes(uint8_t *buffer, size_t size);

/**
 * @brief Constant-time string comparison to prevent timing attacks
 * @param str1 First string to compare
 * @param str2 Second string to compare
 * @param len Length to compare
 * @return true if strings are equal, false otherwise
 */
CXX_C_API bool iris_secure_string_compare(const char *str1, const char *str2, size_t len);

/**
 * @brief Securely clear memory to prevent information leakage
 * @param ptr Pointer to memory to clear
 * @param size Size of memory to clear
 */
CXX_C_API void iris_secure_memzero(void *ptr, size_t size);

/**
 * @brief Get human-readable error message for security result code
 * @param result Security result code
 * @return Human-readable error message
 */
CXX_C_API const char *iris_security_error_string(iris_security_result_t result);

/* ============================================================================
 * Security Context Management
 * ============================================================================ */

/**
 * @brief Initialize a security context structure
 * @param ctx Security context to initialize
 * @return IRIS_SECURITY_OK on success, error code on failure
 */
CXX_C_API iris_security_result_t iris_security_context_init(iris_security_context_t *ctx);

/**
 * @brief Reset security context validation state
 * @param ctx Security context to reset
 */
CXX_C_API void iris_security_context_reset(iris_security_context_t *ctx);

/**
 * @brief Update rate limiting information in security context
 * @param ctx Security context to update
 * @param current_time Current timestamp
 * @return IRIS_SECURITY_OK if within limits, error code if rate limited
 */
CXX_C_API iris_security_result_t iris_security_context_update_rate_limit(iris_security_context_t *ctx, time_t current_time);

/**
 * @brief Mark security context as having suspicious activity
 * @param ctx Security context to update
 * @param threat_level Threat level (0-10, higher is more threatening)
 */
CXX_C_API void iris_security_context_mark_suspicious(iris_security_context_t *ctx, int threat_level);

/* ============================================================================
 * High-Performance re2c-based Validators
 * ============================================================================ */

/**
 * @brief Validate HTTP header name using re2c (optimized)
 * @param name Header name to validate
 * @param len Length of header name
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_http_header_name_re2c(const char *name, size_t len);

/**
 * @brief Validate HTTP header value using re2c (optimized)
 * @param value Header value to validate
 * @param len Length of header value
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_http_header_value_re2c(const char *value, size_t len);

/**
 * @brief Validate cookie name using re2c (optimized)
 * @param name Cookie name to validate
 * @param len Length of cookie name
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_cookie_name_re2c(const char *name, size_t len);

/**
 * @brief Validate cookie value using re2c (optimized)
 * @param value Cookie value to validate
 * @param len Length of cookie value
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_cookie_value_re2c(const char *value, size_t len);

/**
 * @brief Validate URL path using re2c (optimized)
 * @param path URL path to validate
 * @param len Length of path
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
CXX_C_API iris_security_result_t iris_validate_url_path_re2c(const char *path, size_t len);

/**
 * @brief Check for suspicious parameter patterns using re2c (optimized)
 * @param param Parameter to check
 * @param len Length of parameter
 * @return true if suspicious, false otherwise
 */
CXX_C_API bool iris_is_suspicious_parameter_re2c(const char *param, size_t len);

/**
 * @brief Sanitize URL parameter using re2c (optimized)
 * @param param Input parameter
 * @param param_len Length of input parameter
 * @param sanitized Output buffer
 * @param buffer_size Size of output buffer
 * @return IRIS_SECURITY_OK on success, error code otherwise
 */
CXX_C_API iris_security_result_t iris_sanitize_url_parameter_re2c(const char *param, size_t param_len,
                                                                 char *sanitized, size_t buffer_size);

/* ============================================================================
 * JWT (JSON Web Token) Support
 * ============================================================================ */

struct Req;
struct Res;
struct Chain;

/**
 * @brief Middleware for JWT authentication.
 * 
 * Verifies 'Authorization: Bearer <token>' header.
 * If valid, attaches the decoded JWT to request context.
 * 
 * @param req Request object
 * @param res Response object
 * @param chain Middleware chain
 * @return 0 if authorized, non-zero if unauthorized (sends 401)
 */
CXX_C_API int iris_jwt_middleware(struct Req *req, struct Res *res, struct Chain *chain);

/**
 * @brief Set the secret key for JWT verification
 * @param secret Secret key string
 */
CXX_C_API void iris_jwt_set_secret(const char *secret);

/**
 * @brief Generate a JWT token
 * @param secret Secret key for signing
 * @param claims_json JSON string containing claims
 * @return Allocated JWT token string (must be freed by caller), or NULL on failure
 */
CXX_C_API char *iris_jwt_encode(const char *secret, const char *claims_json);

/**
 * @brief Get the decoded JWT object from request context
 * @param req Request object
 * @return Pointer to JWT object (void*), or NULL if not available
 */
CXX_C_API void *iris_jwt_get_claims(struct Req *req);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_SECURITY_H */