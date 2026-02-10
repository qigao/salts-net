#include "tinytest.h"
#include <stdlib.h>
#include <string.h>
#include "rpc.h"

spec("rpc_memory_leak") {
    before_each() {
        // Setup code if needed
    }

    after_each() {
        // Cleanup code if needed
    }

    it("should initialize RPC successfully") {
        rpc_config_t config = {
            .endpoint = "/test-rpc",
            .default_protocol = RPC_PROTOCOL_JSON,
            .enable_introspection = 1,
            .enable_batch = 1,
            .max_batch_size = 10,
            .max_request_size = 1024
        };

        rpc_context_t *ctx = rpc_init(&config);
        check_not_null(ctx);
        check_not_null(ctx->config.endpoint);
        check_str_eq(ctx->config.endpoint, "/test-rpc");
        check_not_null(ctx->methods);
        check_int_eq(ctx->method_capacity, 16);
        check_int_eq(ctx->method_count, 0);

        rpc_destroy(ctx);
    }

    it("should handle NULL config") {
        rpc_context_t *ctx = rpc_init(NULL);
        check_null(ctx);
    }

    it("should handle NULL endpoint") {
        rpc_config_t config = {
            .endpoint = NULL,
            .default_protocol = RPC_PROTOCOL_JSON,
            .enable_introspection = 1,
            .enable_batch = 1,
            .max_batch_size = 10,
            .max_request_size = 1024
        };

        rpc_context_t *ctx = rpc_init(&config);
        check_not_null(ctx);
        check_null(ctx->config.endpoint);

        rpc_destroy(ctx);
    }

    it("should duplicate endpoint string") {
        // Test that the endpoint string is properly duplicated
        char *original_endpoint = malloc(20);
        strcpy(original_endpoint, "/dynamic-endpoint");
        
        rpc_config_t config = {
            .endpoint = original_endpoint,
            .default_protocol = RPC_PROTOCOL_JSON,
            .enable_introspection = 1,
            .enable_batch = 1,
            .max_batch_size = 10,
            .max_request_size = 1024
        };

        rpc_context_t *ctx = rpc_init(&config);
        check_not_null(ctx);
        check_not_null(ctx->config.endpoint);
        check_str_eq(ctx->config.endpoint, "/dynamic-endpoint");
        
        // Verify that the endpoint was duplicated (different memory addresses)
        check_ptr_ne(ctx->config.endpoint, original_endpoint);
        
        // Free the original endpoint to simulate the caller freeing it
        free(original_endpoint);
        
        // The context should still have a valid endpoint
        check_str_eq(ctx->config.endpoint, "/dynamic-endpoint");

        rpc_destroy(ctx);
    }
}