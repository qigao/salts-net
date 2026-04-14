/**
 * @file coro_wss_example.c
 * @brief Coroutine-based WebSocket Secure (WSS) client.
 *
 * Creates a plain TCP socket, then hands it to coro_socket_connect_ws() with
 * is_tls=1.  That single call performs: DNS -> TCP connect -> TLS handshake
 * -> HTTP Upgrade — all while the coroutine is suspended.
 *
 * Usage: ./coro_wss_example
 */

#include "CoroNet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WSS_EXAMPLE_HOST "ws.ifelse.io"
#define WSS_EXAMPLE_PORT 443
#define WSS_EXAMPLE_PATH "/"

static void wss_task(coro_t* co, void* arg) {
    UNUSED(co);
    coro_context_t* ctx = (coro_context_t*)arg;

    printf("[Coro] Creating WebSocket client (WSS)...\n");
    coro_socket_t* client = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);

    coro_socket_set_timeout(client, 15000);

    /* is_tls=1 triggers TLS inside the connect; no separate TLS socket needed. */
    printf("[Coro] Connecting to wss://%s%s ...\n", WSS_EXAMPLE_HOST, WSS_EXAMPLE_PATH);
    int r = coro_socket_connect_ws(client, WSS_EXAMPLE_HOST, WSS_EXAMPLE_PORT, WSS_EXAMPLE_PATH, 1);

    if (r != 0) {
        printf("[Coro] Connection failed: (code %d) %s\n", r, turbo_strerror(r));
        coro_socket_destroy(client);
        return;
    }

    printf("[Coro] Connected! Sending secure message...\n");
    const char* msg = "Hello from TurboNet coro WSS!";
    r = coro_socket_send(client, msg, strlen(msg));

    if (r != 0) {
        printf("[Coro] Send failed: (code %d) %s\n", r, turbo_strerror(r));
        coro_socket_destroy(client);
        return;
    }

    printf("[Coro] Waiting for echo...\n");
    for (int attempt = 0; attempt < 4; ++attempt) {
        char* data = NULL;
        size_t len = 0;
        r = coro_socket_recv(client, &data, &len);

        if (r != 0) {
            printf("[Coro] Recv failed: (code %d) %s\n", r, turbo_strerror(r));
            break;
        }

        if (!data || len == 0) {
            continue;
        }

        char buf[512];
        size_t to_print = len > sizeof(buf) - 1 ? sizeof(buf) - 1 : len;
        memcpy(buf, data, to_print);
        buf[to_print] = '\0';

        if (len == strlen(msg) && memcmp(data, msg, len) == 0) {
            printf("[Coro] Received echo (%zu bytes): %s\n", len, buf);
            coro_socket_free_recv(data);
            break;
        }

        printf("[Coro] Received frame (%zu bytes): %s\n", len, buf);
        coro_socket_free_recv(data);
    }

    coro_socket_destroy(client);
    printf("[Coro] WSS task complete.\n");
}

int main(void) {
    printf("[Main] Initializing context\n");
    coro_context_t* ctx = coro_context_create(NULL);

    printf("[Main] Spawning coroutine\n");
    coro_context_spawn(ctx, wss_task, ctx);

    printf("[Main] Starting loop...\n");
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    printf("[Main] Done.\n");
    return 0;
}
