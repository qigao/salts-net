/**
 * @file coro_client_example.c
 * @brief Prototype of coroutine-based network client.
 */

#include "CoroNet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    coro_context_t* ctx;
    const char* host;
    int port;
} context_t;

static void network_task(coro_t* co, void* arg) {
    UNUSED(co);
    context_t* ctx = (context_t*)arg;
    printf("[Coro] Starting network task for %s:%d\n", ctx->host, ctx->port);

    coro_socket_t* client = coro_socket_create_tcpv4(ctx->ctx);
    if (!client) {
        printf("[Coro] Failed to create client\n");
        return;
    }

    // 1. Connect (yields until done)
    printf("[Coro] Connecting...\n");
    int r = coro_socket_connect(client, ctx->host, ctx->port);
    if (r != 0) {
        printf("[Coro] Connect failed: %s\n", turbo_strerror(r));
        coro_socket_destroy(client);
        return;
    }
    printf("[Coro] Connected!\n");

    // 2. Send request (yields until queued)
    const char* req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    printf("[Coro] Sending request...\n");
    r = coro_socket_send(client, req, strlen(req));
    if (r != 0) {
        printf("[Coro] Send failed: %s\n", turbo_strerror(r));
        coro_socket_destroy(client);
        return;
    }

    // 3. Receive response (yields until data arrives)
    printf("[Coro] Waiting for response...\n");
    char* data = NULL;
    size_t len = 0;
    r = coro_socket_recv(client, &data, &len);
    if (r != 0) {
        printf("[Coro] Recv failed: %s\n", turbo_strerror(r));
    } else {
        printf("[Coro] Received %zu bytes:\n%.*s\n", len, (int)len, data);
        coro_socket_free_recv(data);
    }

    // 4. Cleanup
    printf("[Coro] Task complete, cleaning up.\n");
    coro_socket_destroy(client);
}

int main(int argc, char** argv) {
    const char* host = (argc >= 3) ? argv[1] : "127.0.0.1";
    int port = (argc >= 3) ? atoi(argv[2]) : 8080;

    printf("[Main] Initializing context\n");
    coro_context_t* ctx = coro_context_create(NULL);

    context_t app = { .ctx = ctx, .host = host, .port = port };

    printf("[Main] Spawning coroutine\n");
    coro_context_spawn(ctx, network_task, &app);

    printf("[Main] Starting event loop\n");
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    printf("[Main] Loop exited\n");
    coro_context_destroy(ctx);

    return 0;
}
