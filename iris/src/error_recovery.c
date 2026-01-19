#include "error_recovery.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "tlog.h"

/**
 * @file error_recovery.c
 * @brief Implementation of error handling and recovery system
 */

/* Global error handler */
static iris_error_handler_t g_error_handler = NULL;

/* Default error handler */
static iris_recovery_action_t default_error_handler(const iris_error_context_t *ctx) {
    if (!ctx) {
        return IRIS_RECOVERY_CONTINUE;
    }

    /* Log the error */
    iris_log_error(ctx);

    /* Determine recovery action based on error type */
    switch (ctx->error_type) {
        case IRIS_ERROR_OUT_OF_MEMORY:
        case IRIS_ERROR_MEMORY_CORRUPTION:
            return IRIS_RECOVERY_GRACEFUL_SHUTDOWN;
            
        case IRIS_ERROR_CONNECTION_FAILED:
        case IRIS_ERROR_TIMEOUT:
        case IRIS_ERROR_PROTOCOL_ERROR:
            return IRIS_RECOVERY_CONTINUE;
            
        case IRIS_ERROR_BIND_FAILED:
        case IRIS_ERROR_LISTEN_FAILED:
        case IRIS_ERROR_SERVER_INIT_FAILED:
            return IRIS_RECOVERY_GRACEFUL_SHUTDOWN;
            
        case IRIS_ERROR_INVALID_INPUT:
        case IRIS_ERROR_INJECTION_ATTEMPT:
        case IRIS_ERROR_RATE_LIMIT_EXCEEDED:
        case IRIS_ERROR_SIZE_LIMIT_EXCEEDED:
            return IRIS_RECOVERY_REJECT_REQUEST;
            
        case IRIS_ERROR_INVALID_CONFIG:
        case IRIS_ERROR_MISSING_CONFIG:
            return IRIS_RECOVERY_FALLBACK;
            
        case IRIS_ERROR_HANDLER_FAILED:
        case IRIS_ERROR_MIDDLEWARE_FAILED:
        case IRIS_ERROR_RPC_FAILED:
            return IRIS_RECOVERY_CONTINUE;
            
        default:
            return IRIS_RECOVERY_CONTINUE;
    }
}

int iris_error_recovery_init(void) {
    g_error_handler = default_error_handler;
    return 0;
}

void iris_error_recovery_cleanup(void) {
    g_error_handler = NULL;
}

iris_recovery_action_t iris_handle_error(const iris_error_context_t *ctx, iris_recovery_action_t recovery_action) {
    if (!ctx) {
        return IRIS_RECOVERY_CONTINUE;
    }

    /* Use custom handler if available, otherwise use suggested action */
    if (g_error_handler) {
        iris_recovery_action_t handler_action = g_error_handler(ctx);
        /* Handler can override the suggested action */
        return handler_action;
    }

    /* Log the error */
    iris_log_error(ctx);
    
    return recovery_action;
}

void iris_log_error(const iris_error_context_t *ctx) {
    if (!ctx) {
        return;
    }

    TLOG_ERROR("{}: {} (code: {:d}) at {}:{:d} in {}()",
            iris_error_type_string(ctx->error_type),
            ctx->message,
            ctx->error_code,
            ctx->file ? ctx->file : "unknown",
            ctx->line,
            ctx->function ? ctx->function : "unknown");
}

void iris_set_error_handler(iris_error_handler_t handler) {
    g_error_handler = handler;
}

const char *iris_error_type_string(iris_error_type_t error_type) {
    switch (error_type) {
        case IRIS_ERROR_NONE:
            return "No Error";
        case IRIS_ERROR_INVALID_INPUT:
            return "Invalid Input";
        case IRIS_ERROR_INJECTION_ATTEMPT:
            return "Injection Attempt";
        case IRIS_ERROR_RATE_LIMIT_EXCEEDED:
            return "Rate Limit Exceeded";
        case IRIS_ERROR_SIZE_LIMIT_EXCEEDED:
            return "Size Limit Exceeded";
        case IRIS_ERROR_OUT_OF_MEMORY:
            return "Out of Memory";
        case IRIS_ERROR_BUFFER_OVERFLOW:
            return "Buffer Overflow";
        case IRIS_ERROR_MEMORY_CORRUPTION:
            return "Memory Corruption";
        case IRIS_ERROR_CONNECTION_FAILED:
            return "Connection Failed";
        case IRIS_ERROR_TIMEOUT:
            return "Timeout";
        case IRIS_ERROR_PROTOCOL_ERROR:
            return "Protocol Error";
        case IRIS_ERROR_BIND_FAILED:
            return "Bind Failed";
        case IRIS_ERROR_LISTEN_FAILED:
            return "Listen Failed";
        case IRIS_ERROR_INVALID_CONFIG:
            return "Invalid Configuration";
        case IRIS_ERROR_MISSING_CONFIG:
            return "Missing Configuration";
        case IRIS_ERROR_HANDLER_FAILED:
            return "Handler Failed";
        case IRIS_ERROR_MIDDLEWARE_FAILED:
            return "Middleware Failed";
        case IRIS_ERROR_RPC_FAILED:
            return "RPC Failed";
        case IRIS_ERROR_SERVER_INIT_FAILED:
            return "Server Initialization Failed";
        default:
            return "Unknown Error";
    }
}

iris_error_context_t iris_create_error_context(iris_error_type_t error_type, int error_code,
                                               const char *message, const char *file, int line,
                                               const char *function) {
    iris_error_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    ctx.error_type = error_type;
    ctx.error_code = error_code;
    ctx.file = file;
    ctx.line = line;
    ctx.function = function;
    ctx.timestamp = time(NULL);
    
    if (message) {
        strncpy(ctx.message, message, sizeof(ctx.message) - 1);
        ctx.message[sizeof(ctx.message) - 1] = '\0';
    }
    
    return ctx;
}