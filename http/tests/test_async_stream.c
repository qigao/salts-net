#include "tinytest.h"
#include "http_client_async.h"
#include <string.h>
#include <stdio.h>
#include <platform.h>

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
}

static void on_complete(http_async_request_t *request, http_async_response_t *response, void *user_data) {
    test_ctx_t *ctx = (test_ctx_t *)user_data;
    ctx->status_code = response->status_code;
    ctx->done = 1;
    ctx->is_sse = http_async_response_is_sse(response);
    if (response->error)
        strncpy(ctx->last_error, response->error, sizeof(ctx->last_error) - 1);
}

spec("http async streaming") {

    it("should stream GET from httpbin") {
        http_async_client_t *client = http_async_client_create();
        check_not_null(client);

        test_ctx_t ctx = {0};
        http_async_request_t *req = http_async_stream_get(
            client, "https://httpbin.org/stream/5", on_data, on_complete, &ctx);
        check_not_null(req);

        int timeout_ms = 15000;
        while (!ctx.done && timeout_ms > 0) {
            turbo_sleep_ms(100);
            timeout_ms -= 100;
        }

        check(ctx.done, "request should complete within timeout");
        check_int_eq(ctx.status_code, 200);
        check(ctx.data_chunks > 0, "should receive at least one chunk");
        check(ctx.total_received > 0, "should receive body data");

        http_async_client_destroy(client);
    }

    it("should not accumulate body in stream mode") {
        http_async_client_t *client = http_async_client_create();
        check_not_null(client);

        test_ctx_t ctx = {0};
        http_async_request_t *req = http_async_stream_get(
            client, "https://httpbin.org/bytes/500", on_data, on_complete, &ctx);
        check_not_null(req);

        int timeout_ms = 10000;
        while (!ctx.done && timeout_ms > 0) {
            turbo_sleep_ms(100);
            timeout_ms -= 100;
        }

        check(ctx.done, "request should complete");
        check_int_eq(ctx.status_code, 200);
        check_size_eq(ctx.total_received, 500);

        http_async_client_destroy(client);
    }
}
