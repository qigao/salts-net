#include "tinytest.h"
#include <stdlib.h>
#include <string.h>
#include "rpc.h"

static int noop_rpc_handler(Req *req, Res *res, rpc_request_t *rpc_req, rpc_response_t *rpc_res) {
    (void)req;
    (void)res;
    (void)rpc_req;
    (void)rpc_res;
    return 0;
}

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

    it("should fail to build response with invalid result JSON") {
        mem_pool_t pool;
        check_int_eq(mem_init(&pool, 0), 0);

        rpc_response_t res = {0};
        res.arena = &pool;
        res.result = "{";
        res.id = "1";

        char *output = NULL;
        size_t output_len = 0;
        check_int_eq(rpc_build_response(&res, &output, &output_len), -1);
        check_null(output);
        check_size_eq(output_len, 0);

        mem_destroy(&pool);
    }

    it("should fail to build response with invalid id JSON") {
        mem_pool_t pool;
        check_int_eq(mem_init(&pool, 0), 0);

        rpc_response_t res = {0};
        res.arena = &pool;
        res.result = "null";
        res.id = "{";

        char *output = NULL;
        size_t output_len = 0;
        check_int_eq(rpc_build_response(&res, &output, &output_len), -1);
        check_null(output);
        check_size_eq(output_len, 0);

        mem_destroy(&pool);
    }

    it("should fail setup when introspection method is already registered") {
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

        rpc_method_t duplicate = {0};
        duplicate.name = "rpc.listMethods";
        duplicate.handler = noop_rpc_handler;
        check_int_eq(rpc_register_method(ctx, &duplicate), 0);
        check_int_eq(rpc_setup_endpoint(ctx), -1);

        rpc_destroy(ctx);
    }

    it("should reject rpc request with invalid id type") {
        mem_pool_t pool;
        check_int_eq(mem_init(&pool, 0), 0);

        Req req = {0};
        rpc_request_t rpc_req = {0};
        req.arena = &pool;
        req.body = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":true}";
        req.body_len = strlen(req.body);

        check_int_eq(rpc_parse_request(&req, &rpc_req), RPC_ERROR_INVALID_REQUEST);
        check_null(rpc_req.id);

        mem_destroy(&pool);
    }

    it("should preserve large numeric rpc request id") {
        mem_pool_t pool;
        check_int_eq(mem_init(&pool, 0), 0);

        Req req = {0};
        rpc_request_t rpc_req = {0};
        req.arena = &pool;
        req.body = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":9007199254740991}";
        req.body_len = strlen(req.body);

        check_int_eq(rpc_parse_request(&req, &rpc_req), 0);
        check_not_null(rpc_req.id);
        check_str_eq(rpc_req.id, "9007199254740991");

        mem_destroy(&pool);
    }
}
