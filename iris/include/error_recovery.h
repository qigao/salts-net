#ifndef IRIS_ERROR_RECOVERY_H
#define IRIS_ERROR_RECOVERY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <time.h>

/**
 * @file error_recovery.h
 * @brief Error handling and recovery system for Iris framework
 */

/**
 * @brief Error types classification
 */
typedef enum {
    IRIS_ERROR_NONE = 0,
    
    /* Security errors (1000-1999) */
    IRIS_ERROR_INVALID_INPUT = 1001,
    IRIS_ERROR_INJECTION_ATTEMPT = 1002,
    IRIS_ERROR_RATE_LIMIT_EXCEEDED = 1003,
    IRIS_ERROR_SIZE_LIMIT_EXCEEDED = 1004,
    
    /* Memory errors (2000-2999) */
    IRIS_ERROR_OUT_OF_MEMORY = 2001,
    IRIS_ERROR_BUFFER_OVERFLOW = 2002,
    IRIS_ERROR_MEMORY_CORRUPTION = 2003,
    
    /* Network errors (3000-3999) */
    IRIS_ERROR_CONNECTION_FAILED = 3001,
    IRIS_ERROR_TIMEOUT = 3002,
    IRIS_ERROR_PROTOCOL_ERROR = 3003,
    IRIS_ERROR_BIND_FAILED = 3004,
    IRIS_ERROR_LISTEN_FAILED = 3005,
    
    /* Configuration errors (4000-4999) */
    IRIS_ERROR_INVALID_CONFIG = 4001,
    IRIS_ERROR_MISSING_CONFIG = 4002,
    
    /* Application errors (5000-5999) */
    IRIS_ERROR_HANDLER_FAILED = 5001,
    IRIS_ERROR_MIDDLEWARE_FAILED = 5002,
    IRIS_ERROR_RPC_FAILED = 5003,
    IRIS_ERROR_SERVER_INIT_FAILED = 5004
} iris_error_type_t;

/**
 * @brief Recovery action types
 */
typedef enum {
    IRIS_RECOVERY_NONE = 0,
    IRIS_RECOVERY_RETRY,
    IRIS_RECOVERY_FALLBACK,
    IRIS_RECOVERY_GRACEFUL_SHUTDOWN,
    IRIS_RECOVERY_CONTINUE,
    IRIS_RECOVERY_REJECT_REQUEST
} iris_recovery_action_t;

/**
 * @brief Error context structure
 */
typedef struct iris_error_context {
    iris_error_type_t error_type;
    int error_code;
    char message[256];
    const char *file;
    int line;
    const char *function;
    time_t timestamp;
    void *user_data;
} iris_error_context_t;

/**
 * @brief Error handler callback function type
 */
typedef iris_recovery_action_t (*iris_error_handler_t)(const iris_error_context_t *ctx);

/**
 * @brief Initialize error recovery system
 * @return 0 on success, negative error code on failure
 */
int iris_error_recovery_init(void);

/**
 * @brief Cleanup error recovery system
 */
void iris_error_recovery_cleanup(void);

/**
 * @brief Handle an error with recovery
 * @param ctx Error context
 * @param recovery_action Suggested recovery action
 * @return Final recovery action to take
 */
iris_recovery_action_t iris_handle_error(const iris_error_context_t *ctx, iris_recovery_action_t recovery_action);

/**
 * @brief Log an error
 * @param ctx Error context
 */
void iris_log_error(const iris_error_context_t *ctx);

/**
 * @brief Set custom error handler
 * @param handler Error handler callback
 */
void iris_set_error_handler(iris_error_handler_t handler);

/**
 * @brief Get human-readable error message
 * @param error_type Error type
 * @return Error message string
 */
const char *iris_error_type_string(iris_error_type_t error_type);

/**
 * @brief Create error context
 * @param error_type Error type
 * @param error_code Specific error code
 * @param message Error message
 * @param file Source file name
 * @param line Source line number
 * @param function Function name
 * @return Initialized error context
 */
iris_error_context_t iris_create_error_context(iris_error_type_t error_type, int error_code,
                                               const char *message, const char *file, int line,
                                               const char *function);

/**
 * @brief Macro to create error context with current location
 */
#define IRIS_ERROR_CONTEXT(type, code, msg) \
    iris_create_error_context(type, code, msg, __FILE__, __LINE__, __func__)

/**
 * @brief Macro to handle error with context
 */
#define IRIS_HANDLE_ERROR(type, code, msg, recovery) \
    do { \
        iris_error_context_t ctx = IRIS_ERROR_CONTEXT(type, code, msg); \
        iris_handle_error(&ctx, recovery); \
    } while(0)

#ifdef __cplusplus
}
#endif

#endif /* IRIS_ERROR_RECOVERY_H */