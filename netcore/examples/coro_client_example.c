/**
 * @file coro_client_example.c
 * @brief Prototype of coroutine-based network client.
 */

#include "turbo_coro_client.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    turbo_coro_context_t* ctx;
    const char* url;
} context_t;

static void network_task(turbo_coro_t* co, void* arg) {
    UNUSED(co);
    context_t* ctx = (context_t*)arg;
    printf("[Coro] Starting network task for %s\n", ctx->url);

    turbo_coro_client_t* client = turbo_coro_client_create(ctx->ctx);
    if (!client) {
        printf("[Coro] Failed to create client\n");
        return;
    }

    // 1. Connect (yields until done)
    printf("[Coro] Connecting...\n");
    int r = turbo_coro_client_connect(client, ctx->url);
    if (r != 0) {
        printf("[Coro] Connect failed: %s\n", turbo_strerror(r));
        turbo_coro_client_destroy(client);
        return;
    }
    printf("[Coro] Connected!\n");

    // 2. Send request (yields until queued)
    const char* req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    printf("[Coro] Sending request...\n");
    r = turbo_coro_client_send(client, req, strlen(req));
    if (r != 0) {
        printf("[Coro] Send failed: %s\n", turbo_strerror(r));
        turbo_coro_client_destroy(client);
        return;
    }

    // 3. Receive response (yields until data arrives)
    printf("[Coro] Waiting for response...\n");
    char* data = NULL;
    size_t len = 0;
    r = turbo_coro_client_recv(client, &data, &len);
    if (r != 0) {
        printf("[Coro] Recv failed: %s\n", turbo_strerror(r));
    } else {
        printf("[Coro] Received %zu bytes:\n%.*s\n", len, (int)len, data);
        if (data) free(data);
    }

    // 4. Cleanup
    printf("[Coro] Task complete, cleaning up.\n");
    turbo_coro_client_destroy(client);
}

int main(int argc, char** argv) {
    const char* url = (argc > 1) ? argv[1] : "tcp://127.0.0.1:8080";

    printf("[Main] Initializing context\n");
    turbo_coro_context_t* ctx = turbo_coro_context_create(NULL);

    context_t app = { .ctx = ctx, .url = url };

    printf("[Main] Creating coroutine\n");
    turbo_coro_t* co = turbo_coro_create(network_task, &app, NULL);

    printf("[Main] Starting coroutine...\n");
    turbo_coro_resume(co);

    printf("[Main] Coroutine yielded, starting event loop\n");
    turbo_coro_context_run(ctx, TURBO_RUN_DEFAULT);

    printf("[Main] Loop exited, destroying coroutine\n");
    turbo_coro_destroy(co);
    turbo_coro_context_destroy(ctx);

    return 0;
}
