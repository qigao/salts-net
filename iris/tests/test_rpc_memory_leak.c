#include <unity.h>
#include <stdlib.h>
#include <string.h>
#include "rpc.h"

void setUp(void) {
    // Setup code if needed
}

void tearDown(void) {
    // Cleanup code if needed
}

void test_rpc_init_success(void) {
    rpc_config_t config = {
        .endpoint = "/test-rpc",
        .default_protocol = RPC_PROTOCOL_JSON,
        .enable_introspection = 1,
        .enable_batch = 1,
        .max_batch_size = 10,
        .max_request_size = 1024
    };

    rpc_context_t *ctx = rpc_init(&config);
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_NOT_NULL(ctx->config.endpoint);
    TEST_ASSERT_EQUAL_STRING("/test-rpc", ctx->config.endpoint);
    TEST_ASSERT_NOT_NULL(ctx->methods);
    TEST_ASSERT_EQUAL(16, ctx->method_capacity);
    TEST_ASSERT_EQUAL(0, ctx->method_count);

    rpc_destroy(ctx);
}

void test_rpc_init_null_config(void) {
    rpc_context_t *ctx = rpc_init(NULL);
    TEST_ASSERT_NULL(ctx);
}

void test_rpc_init_null_endpoint(void) {
    rpc_config_t config = {
        .endpoint = NULL,
        .default_protocol = RPC_PROTOCOL_JSON,
        .enable_introspection = 1,
        .enable_batch = 1,
        .max_batch_size = 10,
        .max_request_size = 1024
    };

    rpc_context_t *ctx = rpc_init(&config);
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_NULL(ctx->config.endpoint);

    rpc_destroy(ctx);
}

void test_rpc_init_endpoint_duplication(void) {
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
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_NOT_NULL(ctx->config.endpoint);
    TEST_ASSERT_EQUAL_STRING("/dynamic-endpoint", ctx->config.endpoint);
    
    // Verify that the endpoint was duplicated (different memory addresses)
    TEST_ASSERT_NOT_EQUAL(original_endpoint, ctx->config.endpoint);
    
    // Free the original endpoint to simulate the caller freeing it
    free(original_endpoint);
    
    // The context should still have a valid endpoint
    TEST_ASSERT_EQUAL_STRING("/dynamic-endpoint", ctx->config.endpoint);

    rpc_destroy(ctx);
}

int main(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_rpc_init_success);
    RUN_TEST(test_rpc_init_null_config);
    RUN_TEST(test_rpc_init_null_endpoint);
    RUN_TEST(test_rpc_init_endpoint_duplication);
    
    return UNITY_END();
}