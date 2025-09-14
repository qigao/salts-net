#ifndef IRIS_CONFIG_H
#define IRIS_CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file config.h
 * @brief Production-ready configuration management for Iris framework
 * 
 * This module provides comprehensive configuration management including
 * security limits, timeouts, memory management, logging, and TLS settings
 * suitable for production deployment.
 */

/* Configuration error codes */
typedef enum {
    IRIS_CONFIG_OK = 0,
    IRIS_CONFIG_ERROR_INVALID_VALUE = -1,
    IRIS_CONFIG_ERROR_OUT_OF_RANGE = -2,
    IRIS_CONFIG_ERROR_MISSING_REQUIRED = -3,
    IRIS_CONFIG_ERROR_FILE_NOT_FOUND = -4,
    IRIS_CONFIG_ERROR_PARSE_ERROR = -5,
    IRIS_CONFIG_ERROR_NULL_POINTER = -6,
    IRIS_CONFIG_ERROR_BUFFER_TOO_SMALL = -7
} iris_config_result_t;

/* Log levels */
typedef enum {
    IRIS_LOG_LEVEL_NONE = 0,
    IRIS_LOG_LEVEL_ERROR = 1,
    IRIS_LOG_LEVEL_WARN = 2,
    IRIS_LOG_LEVEL_INFO = 3,
    IRIS_LOG_LEVEL_DEBUG = 4,
    IRIS_LOG_LEVEL_TRACE = 5
} iris_log_level_t;

/* TLS version constants */
typedef enum {
    IRIS_TLS_VERSION_1_0 = 10,
    IRIS_TLS_VERSION_1_1 = 11,
    IRIS_TLS_VERSION_1_2 = 12,
    IRIS_TLS_VERSION_1_3 = 13
} iris_tls_version_t;

/**
 * @brief Main configuration structure for Iris framework
 * 
 * Contains all configurable parameters for production deployment including
 * security limits, performance tuning, logging, and TLS settings.
 */
typedef struct iris_config {
    /* Security limits */
    size_t max_request_size;        /**< Maximum HTTP request body size in bytes */
    size_t max_header_size;         /**< Maximum HTTP header size in bytes */
    size_t max_url_length;          /**< Maximum URL length in characters */
    int max_headers_count;          /**< Maximum number of HTTP headers per request */
    
    /* Rate limiting */
    int requests_per_second;        /**< Maximum requests per second per client */
    int connections_per_ip;         /**< Maximum concurrent connections per IP */
    
    /* Timeouts (in seconds) */
    int connection_timeout;         /**< Connection establishment timeout */
    int request_timeout;            /**< Request processing timeout */
    int keepalive_timeout;          /**< Keep-alive connection timeout */
    
    /* Memory management */
    size_t arena_initial_size;      /**< Initial arena allocation size */
    size_t arena_max_size;          /**< Maximum arena size per request */
    
    /* Logging */
    iris_log_level_t log_level;     /**< Logging level */
    char log_format[64];            /**< Log format string */
    
    /* TLS settings */
    char cipher_suites[256];        /**< Allowed TLS cipher suites */
    iris_tls_version_t min_tls_version; /**< Minimum TLS version */
    
    /* Server settings */
    int max_concurrent_connections; /**< Maximum total concurrent connections */
    int worker_threads;             /**< Number of worker threads */
    bool enable_compression;        /**< Enable HTTP compression */
    
    /* Health check settings */
    bool enable_health_check;       /**< Enable health check endpoint */
    char health_check_path[128];    /**< Health check endpoint path */
    
    /* Security features */
    bool enable_csrf_protection;    /**< Enable CSRF protection */
    bool enable_xss_protection;     /**< Enable XSS protection headers */
    bool enable_content_security_policy; /**< Enable CSP headers */
    
    /* File upload settings */
    size_t max_file_upload_size;    /**< Maximum file upload size */
    char allowed_file_extensions[512]; /**< Comma-separated allowed file extensions */
    
    /* CORS settings */
    bool enable_cors;               /**< Enable CORS support */
    char cors_allowed_origins[256]; /**< Allowed CORS origins */
    char cors_allowed_methods[128]; /**< Allowed CORS methods */
    char cors_allowed_headers[256]; /**< Allowed CORS headers */
    
    /* Performance tuning */
    size_t read_buffer_size;        /**< Network read buffer size */
    size_t write_buffer_size;       /**< Network write buffer size */
    int tcp_nodelay;                /**< TCP_NODELAY socket option */
    int tcp_keepalive;              /**< TCP keepalive option */
} iris_config_t;

/**
 * @brief Default configuration values
 * 
 * Provides sensible defaults for production deployment with security-first
 * approach and reasonable performance characteristics.
 */
extern const iris_config_t IRIS_DEFAULT_CONFIG;

/* ============================================================================
 * Configuration Management Functions
 * ============================================================================ */

/**
 * @brief Create a new configuration with default values
 * @return New configuration instance with defaults
 */
CXX_C_API iris_config_t *iris_config_create_default(void);

/**
 * @brief Create a configuration by copying another configuration
 * @param source Source configuration to copy
 * @return New configuration instance or NULL on failure
 */
CXX_C_API iris_config_t *iris_config_copy(const iris_config_t *source);

/**
 * @brief Destroy a configuration instance
 * @param config Configuration to destroy
 */
CXX_C_API void iris_config_destroy(iris_config_t *config);

/**
 * @brief Load configuration from file
 * @param filename Path to configuration file
 * @param config Configuration structure to populate
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_load_from_file(const char *filename, iris_config_t *config);

/**
 * @brief Save configuration to file
 * @param filename Path to configuration file
 * @param config Configuration to save
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_save_to_file(const char *filename, const iris_config_t *config);

/**
 * @brief Load configuration from environment variables
 * @param config Configuration structure to populate
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_load_from_env(iris_config_t *config);

/**
 * @brief Parse configuration from JSON string
 * @param json_string JSON configuration string
 * @param config Configuration structure to populate
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_parse_json(const char *json_string, iris_config_t *config);

/**
 * @brief Convert configuration to JSON string
 * @param config Configuration to convert
 * @param json_buffer Buffer to store JSON string
 * @param buffer_size Size of JSON buffer
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_to_json(const iris_config_t *config, 
                                                   char *json_buffer, size_t buffer_size);

/* ============================================================================
 * Configuration Validation
 * ============================================================================ */

/**
 * @brief Validate configuration values
 * @param config Configuration to validate
 * @return IRIS_CONFIG_OK if valid, error code otherwise
 */
CXX_C_API iris_config_result_t iris_config_validate(const iris_config_t *config);

/**
 * @brief Check if a configuration value is within acceptable range
 * @param value Value to check
 * @param min_value Minimum acceptable value
 * @param max_value Maximum acceptable value
 * @return true if value is in range, false otherwise
 */
CXX_C_API bool iris_config_value_in_range(int value, int min_value, int max_value);

/**
 * @brief Check if a size value is within acceptable range
 * @param value Size value to check
 * @param min_value Minimum acceptable size
 * @param max_value Maximum acceptable size
 * @return true if value is in range, false otherwise
 */
CXX_C_API bool iris_config_size_in_range(size_t value, size_t min_value, size_t max_value);

/**
 * @brief Validate TLS cipher suite string
 * @param cipher_suites Cipher suite string to validate
 * @return IRIS_CONFIG_OK if valid, error code otherwise
 */
CXX_C_API iris_config_result_t iris_config_validate_cipher_suites(const char *cipher_suites);

/**
 * @brief Validate log format string
 * @param log_format Log format string to validate
 * @return IRIS_CONFIG_OK if valid, error code otherwise
 */
CXX_C_API iris_config_result_t iris_config_validate_log_format(const char *log_format);

/* ============================================================================
 * Configuration Getters and Setters
 * ============================================================================ */

/**
 * @brief Set security limits in configuration
 * @param config Configuration to modify
 * @param max_request_size Maximum request size
 * @param max_header_size Maximum header size
 * @param max_url_length Maximum URL length
 * @param max_headers_count Maximum headers count
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_set_security_limits(iris_config_t *config,
                                                              size_t max_request_size,
                                                              size_t max_header_size,
                                                              size_t max_url_length,
                                                              int max_headers_count);

/**
 * @brief Set rate limiting configuration
 * @param config Configuration to modify
 * @param requests_per_second Maximum requests per second
 * @param connections_per_ip Maximum connections per IP
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_set_rate_limits(iris_config_t *config,
                                                          int requests_per_second,
                                                          int connections_per_ip);

/**
 * @brief Set timeout configuration
 * @param config Configuration to modify
 * @param connection_timeout Connection timeout in seconds
 * @param request_timeout Request timeout in seconds
 * @param keepalive_timeout Keep-alive timeout in seconds
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_set_timeouts(iris_config_t *config,
                                                       int connection_timeout,
                                                       int request_timeout,
                                                       int keepalive_timeout);

/**
 * @brief Set memory management configuration
 * @param config Configuration to modify
 * @param arena_initial_size Initial arena size
 * @param arena_max_size Maximum arena size
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_set_memory_limits(iris_config_t *config,
                                                           size_t arena_initial_size,
                                                           size_t arena_max_size);

/**
 * @brief Set logging configuration
 * @param config Configuration to modify
 * @param log_level Logging level
 * @param log_format Log format string
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_set_logging(iris_config_t *config,
                                                      iris_log_level_t log_level,
                                                      const char *log_format);

/**
 * @brief Set TLS configuration
 * @param config Configuration to modify
 * @param cipher_suites Cipher suites string
 * @param min_tls_version Minimum TLS version
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_set_tls(iris_config_t *config,
                                                   const char *cipher_suites,
                                                   iris_tls_version_t min_tls_version);

/* ============================================================================
 * Runtime Configuration Updates
 * ============================================================================ */

/**
 * @brief Check if a configuration parameter can be updated at runtime
 * @param parameter_name Name of the configuration parameter
 * @return true if parameter can be updated at runtime, false otherwise
 */
CXX_C_API bool iris_config_can_update_runtime(const char *parameter_name);

/**
 * @brief Update configuration parameter at runtime
 * @param config Configuration to update
 * @param parameter_name Name of parameter to update
 * @param value New value as string
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_update_runtime(iris_config_t *config,
                                                         const char *parameter_name,
                                                         const char *value);

/**
 * @brief Apply configuration changes to running server
 * @param config New configuration to apply
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_apply_changes(const iris_config_t *config);

/* ============================================================================
 * Configuration Utilities
 * ============================================================================ */

/**
 * @brief Get human-readable error message for configuration result code
 * @param result Configuration result code
 * @return Human-readable error message
 */
CXX_C_API const char *iris_config_error_string(iris_config_result_t result);

/**
 * @brief Print configuration to stdout for debugging
 * @param config Configuration to print
 */
CXX_C_API void iris_config_print(const iris_config_t *config);

/**
 * @brief Get configuration parameter as string
 * @param config Configuration to query
 * @param parameter_name Name of parameter
 * @param buffer Buffer to store parameter value
 * @param buffer_size Size of buffer
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_get_parameter_string(const iris_config_t *config,
                                                               const char *parameter_name,
                                                               char *buffer,
                                                               size_t buffer_size);

/**
 * @brief Set configuration parameter from string
 * @param config Configuration to modify
 * @param parameter_name Name of parameter
 * @param value Parameter value as string
 * @return IRIS_CONFIG_OK on success, error code on failure
 */
CXX_C_API iris_config_result_t iris_config_set_parameter_string(iris_config_t *config,
                                                               const char *parameter_name,
                                                               const char *value);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_CONFIG_H */