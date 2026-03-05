/**
 * @file coro_server_example.c
 * @brief Prototype of coroutine-based network server.
 */

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

#include "turbo_coro_server.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Coroutine handler for each client connection.
 */
static void client_handler(coro_client_t* client, void* arg) {
    UNUSED(arg);
    printf("[Coro] New connection handled in coroutine %p\n", (void*)coro_running());

    char* data = NULL;
    size_t len = 0;

    // Echo loop: receive then send
    while (coro_client_recv(client, &data, &len) == 0) {
        if (len == 0) break; // EOF

        printf("[Coro] Client %p sent: %.*s", (void*)coro_running(), (int)len, data);

        // Echo back
        coro_client_send(client, "Echo: ", 6);
        coro_client_send(client, data, len);

        free(data);
        data = NULL;
    }

    printf("[Coro] Client %p disconnected.\n", (void*)coro_running());
}

int main(int argc, char** argv) {
    const char* url = (argc > 1) ? argv[1] : "tcp://0.0.0.0:8080";

    printf("[Main] Initializing context\n");
    coro_context_t* ctx = coro_context_create(NULL);

    printf("[Main] Creating Coro Server\n");
    coro_server_t* server = coro_server_create(ctx);
    if (!server) {
        printf("[Main] Failed to create server\n");
        coro_context_destroy(ctx);
        return 1;
    }

    printf("[Main] Starting Coro Server on %s\n", url);
    int r = coro_server_listen(server, url, client_handler, NULL);
    if (r != 0) {
        printf("[Main] Server failed to start: %s\n", turbo_strerror(r));
        coro_server_destroy(server);
        coro_context_destroy(ctx);
        return 1;
    }

    printf("[Main] Server listening. Connect using 'nc 127.0.0.1 8080'\n");
    printf("[Main] Press Ctrl+C to stop.\n");

    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    printf("[Main] Stopping server...\n");
    coro_server_destroy(server);
    coro_context_destroy(ctx);

    return 0;
}
