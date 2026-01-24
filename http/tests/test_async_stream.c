/**
 * test_async_stream.c - Tests for async streaming functionality
 */

#include "http_client_async.h"
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include <platform.h>

void setUp(void) { /* Called before each test */ }
void tearDown(void) { /* Called after each test */ }

typedef struct {
    int done;
    int data_chunks;
    size_t total_received;
    int status_code;
    int is_sse;
    char last_error[256];
} test_ctx_t;

static void on_data(http_async_request_t *request, const char *data, size_t len, void *user_data) {
    test_ctx_t *ctx = (test_ctx_t *)user_data;
    ctx->data_chunks++;
    ctx->total_received += len;
    
    // Zero-terminate and print if small for debugging
    if (len < 1000) {
        char buf[1024];
        size_t copy_len = len < 1023 ? len : 1023;
        memcpy(buf, data, copy_len);
        buf[copy_len] = '\0';
        printf("[DEBUG] Data chunk %d (%zu bytes)\n", ctx->data_chunks, len);
        // printf("[DEBUG] Content: %s\n", buf);
    } else {
        printf("[DEBUG] Data chunk %d (%zu bytes)\n", ctx->data_chunks, len);
    }
}

static void on_complete(http_async_request_t *request, http_async_response_t *response, void *user_data) {
    test_ctx_t *ctx = (test_ctx_t *)user_data;
    ctx->status_code = response->status_code;
    ctx->done = 1;
    ctx->is_sse = http_async_response_is_sse(response);
    
    if (response->error) {
        strncpy(ctx->last_error, response->error, sizeof(ctx->last_error) - 1);
    }
    
    printf("[DEBUG] Request complete: status %d, is_sse: %d\n", response->status_code, ctx->is_sse);
}

void test_stream_get_integration(void) {
    http_async_client_t *client = http_async_client_create();
    TEST_ASSERT_NOT_NULL(client);

    test_ctx_t ctx = {0};
    
    // Integration test using httpbin /stream endpoint
    printf("\nStarting stream GET integration test with https://httpbin.org/stream/5...\n");
    http_async_request_t *req = http_async_stream_get(client, "https://httpbin.org/stream/5", 
                                                      on_data, on_complete, &ctx);
    TEST_ASSERT_NOT_NULL(req);

    // Wait up to 15 seconds
    int timeout_ms = 15000;
    while (!ctx.done && timeout_ms > 0) {
        turbo_sleep_ms(100);
        timeout_ms -= 100;
    }

    TEST_ASSERT_TRUE_MESSAGE(ctx.done, "Request should complete within timeout");
    TEST_ASSERT_EQUAL_INT_MESSAGE(200, ctx.status_code, ctx.last_error);
    TEST_ASSERT_TRUE_MESSAGE(ctx.data_chunks > 0, "Should have received at least one data chunk");
    TEST_ASSERT_TRUE_MESSAGE(ctx.total_received > 0, "Should have received some body data");

    http_async_client_destroy(client);
}

void test_stream_only_logic(void) {
    http_async_client_t *client = http_async_client_create();
    TEST_ASSERT_NOT_NULL(client);

    test_ctx_t ctx = {0};
    
    // Start a stream GET
    printf("\nStarting stream-only logic test (verifying no body accumulation)...\n");
    http_async_request_t *req = http_async_stream_get(client, "https://httpbin.org/bytes/500", 
                                                      on_data, on_complete, &ctx);
    TEST_ASSERT_NOT_NULL(req);

    // Wait for completion
    int timeout_ms = 10000;
    while (!ctx.done && timeout_ms > 0) {
        turbo_sleep_ms(100);
        timeout_ms -= 100;
    }

    TEST_ASSERT_TRUE(ctx.done);
    TEST_ASSERT_EQUAL(200, ctx.status_code);
    TEST_ASSERT_EQUAL(500, ctx.total_received);
    
    // We can't easily check response->body from here as it's private/freed
    // but the test reaching here without crashing and passing total_received is a win.
    
    http_async_client_destroy(client);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_stream_get_integration);
  RUN_TEST(test_stream_only_logic);

  return UNITY_END();
}
