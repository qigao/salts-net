#include "tinytest.h"
#include "error_recovery.h"
#include <string.h>

/* Custom error handler for testing */
static iris_recovery_action_t test_error_handler(const iris_error_context_t *ctx) {
    (void)ctx; /* Unused parameter */
    return IRIS_RECOVERY_FALLBACK;
}

spec("error_recovery") {
    before_each() {
        iris_error_recovery_init();
    }

    after_each() {
        iris_error_recovery_cleanup();
    }

    it("should initialize error recovery") {
        int result = iris_error_recovery_init();
        check_int_eq(result, 0);
        iris_error_recovery_cleanup();
    }

    it("should create error context") {
        iris_error_context_t ctx = iris_create_error_context(
            IRIS_ERROR_OUT_OF_MEMORY, -1, "Test error message", 
            __FILE__, __LINE__, __func__);
        
        check_int_eq(ctx.error_type, IRIS_ERROR_OUT_OF_MEMORY);
        check_int_eq(ctx.error_code, -1);
        check_str_eq(ctx.message, "Test error message");
        check_not_null(ctx.file);
        check_true(ctx.line > 0);
        check_not_null(ctx.function);
    }

    it("should return error type string") {
        check_str_eq(iris_error_type_string(IRIS_ERROR_OUT_OF_MEMORY), "Out of Memory");
        check_str_eq(iris_error_type_string(IRIS_ERROR_BIND_FAILED), "Bind Failed");
        check_str_eq(iris_error_type_string(IRIS_ERROR_LISTEN_FAILED), "Listen Failed");
        check_str_eq(iris_error_type_string(IRIS_ERROR_SERVER_INIT_FAILED), "Server Initialization Failed");
    }

    it("should handle memory error") {
        iris_error_context_t ctx = IRIS_ERROR_CONTEXT(IRIS_ERROR_OUT_OF_MEMORY, -1, "Memory allocation failed");
        iris_recovery_action_t action = iris_handle_error(&ctx, IRIS_RECOVERY_GRACEFUL_SHUTDOWN);
        
        /* Memory errors should trigger graceful shutdown */
        check_int_eq(action, IRIS_RECOVERY_GRACEFUL_SHUTDOWN);
    }

    it("should handle network error") {
        iris_error_context_t ctx = IRIS_ERROR_CONTEXT(IRIS_ERROR_CONNECTION_FAILED, -1, "Connection failed");
        iris_recovery_action_t action = iris_handle_error(&ctx, IRIS_RECOVERY_CONTINUE);
        
        /* Network errors should allow continuation */
        check_int_eq(action, IRIS_RECOVERY_CONTINUE);
    }

    it("should handle security error") {
        iris_error_context_t ctx = IRIS_ERROR_CONTEXT(IRIS_ERROR_INVALID_INPUT, -1, "Invalid input detected");
        iris_recovery_action_t action = iris_handle_error(&ctx, IRIS_RECOVERY_REJECT_REQUEST);
        
        /* Security errors should reject the request */
        check_int_eq(action, IRIS_RECOVERY_REJECT_REQUEST);
    }

    it("should shut down on invalid configuration") {
        iris_error_context_t ctx = IRIS_ERROR_CONTEXT(IRIS_ERROR_INVALID_CONFIG, -1, "Bad config");
        iris_recovery_action_t action = iris_handle_error(&ctx, IRIS_RECOVERY_FALLBACK);

        check_int_eq(action, IRIS_RECOVERY_GRACEFUL_SHUTDOWN);
    }

    it("should reject request on handler failure") {
        iris_error_context_t ctx = IRIS_ERROR_CONTEXT(IRIS_ERROR_HANDLER_FAILED, -1, "Handler exploded");
        iris_recovery_action_t action = iris_handle_error(&ctx, IRIS_RECOVERY_CONTINUE);

        check_int_eq(action, IRIS_RECOVERY_REJECT_REQUEST);
    }

    it("should use custom error handler") {
        iris_set_error_handler(test_error_handler);
        
        iris_error_context_t ctx = IRIS_ERROR_CONTEXT(IRIS_ERROR_OUT_OF_MEMORY, -1, "Test error");
        iris_recovery_action_t action = iris_handle_error(&ctx, IRIS_RECOVERY_GRACEFUL_SHUTDOWN);
        
        /* Custom handler should override default behavior */
        check_int_eq(action, IRIS_RECOVERY_FALLBACK);
        
        /* Reset to default handler */
        iris_set_error_handler(NULL);
    }
}
