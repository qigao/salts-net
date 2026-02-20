/**
 * @file test_sse_client.c
 * @brief C client for testing Server-Sent Events (SSE) and RPC streaming
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <platform.h>
#include <turbo_thread.h>
#include <http_client_async.h>
#include <rpc_client.h>

/* --- 1. Standard SSE Client (using http_client_async) --- */

static void on_http_data(http_async_request_t *request, const char *data, size_t len, void *user_data) {
    (void)request;
    (void)user_data;
    
    /* In SSE, data comes in line by line or in blocks */
    /* For this test, we just print whatever we get */
    char *buf = malloc(len + 1);
    if (buf) {
        memcpy(buf, data, len);
        buf[len] = '\0';
        printf("[HTTP SSE] %s", buf);
        free(buf);
    }
}

static void on_http_complete(http_async_request_t *request, http_async_response_t *response, void *user_data) {
    (void)request;
    int *done = (int *)user_data;
    printf("\n[HTTP SSE] Stream closed. Status: %d\n", response->status_code);
    *done = 1;
}

void run_http_sse_test() {
    http_async_client_t *client = http_async_client_create();
    int done = 0;
    
    printf("\n=== HTTP SSE GET Test ===\n");
    printf("Connecting to http://localhost:8080/stream...\n");
    
    http_async_sse_get(client, "http://localhost:8080/stream", on_http_data, on_http_complete, &done);
    
    while (!done) {
        turbo_sleep_ms(100);
    }
    
    http_async_client_destroy(client);
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
    int *done = (int *)user_data;
    printf("[RPC SSE] Call complete. Status: %d\n", result->http_status);
    *done = 1;
}

void run_rpc_sse_test() {
    rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("http://localhost:8080/rpc");
    rpc_client_t *client = rpc_client_create(&config);
    int done = 0;
    
    printf("\n=== RPC SSE POST Test ===\n");
    printf("Calling math.count(n=5) on http://localhost:8080/rpc...\n");
    
    rpc_client_call_stream(client, "math.count", "{\"n\":5}", on_rpc_result, on_rpc_complete, &done);
    
    while (!done) {
        turbo_sleep_ms(100);
    }
    
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
