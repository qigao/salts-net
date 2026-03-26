/**
 * @file test_sse_client.c
 * @brief C client for testing Server-Sent Events (SSE) and RPC streaming
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <platform.h>
#include "CoroNet.h"
#include <turbo_coro.h>
#include <http_client.h>
#include <rpc_client.h>

/* --- 1. SSE Client (using http_client) --- */

static void on_sse_data(const char *data, size_t len, void *user_data) {
    (void)user_data;
    char *buf = malloc(len + 1);
    if (buf) {
        memcpy(buf, data, len);
        buf[len] = '\0';
        printf("[HTTP SSE] %s", buf);
        free(buf);
    }
}

static void run_http_sse_once(void) {
    http_client_t *client = http_client_create(NULL);
    http_client_set_timeout(client, 30000);

    printf("\n=== HTTP SSE GET Test ===\n");
    printf("Connecting to http://localhost:8080/stream...\n");

    http_response_t *r = http_sse_get(
        client, "http://localhost:8080/stream", on_sse_data, NULL);

    if (r) {
        printf("\n[HTTP SSE] Stream closed. Status: %d\n", r->status_code);
        http_response_free(r);
    }

    http_client_destroy(client);
}

void run_http_sse_test() {
    run_http_sse_once();
}

/* --- 2. RPC SSE Client (using rpc_client) --- */

static void on_rpc_result(rpc_call_result_t *result, void *user_data) {
    (void)user_data;
    if (result->success) {
        printf("[RPC SSE] Result: %s\n", result->result);
    } else {
        printf("[RPC SSE] Error %d: %s\n", result->error_code, result->error_message);
    }
}

static void on_rpc_complete(rpc_call_result_t *result, void *user_data) {
    (void)user_data;
    printf("[RPC SSE] Call complete. Status: %d\n", result->http_status);
}

void run_rpc_sse_test() {
    rpc_client_t *client = rpc_client_create_simple("http://localhost:8080/rpc");

    if (!client) {
        printf("[RPC SSE] Failed to create RPC client\n");
        return;
    }

    printf("\n=== RPC SSE POST Test ===\n");
    printf("Calling math.count(n=5) on http://localhost:8080/rpc...\n");

    rpc_client_call_stream(client, "math.count", "{\"n\":5}", on_rpc_result, on_rpc_complete, NULL);

    rpc_client_destroy(client);
}

int main(int argc, char *argv[]) {
    int mode = 0; // 0=both, 1=http, 2=rpc

    if (argc > 1) {
        if (strcmp(argv[1], "http") == 0) mode = 1;
        else if (strcmp(argv[1], "rpc") == 0) mode = 2;
    }

    printf("TurboNet SSE C Client\n");
    printf("=====================\n");

    if (mode == 0 || mode == 1) {
        run_http_sse_test();
    }

    if (mode == 0 || mode == 2) {
        run_rpc_sse_test();
    }

    return 0;
}
