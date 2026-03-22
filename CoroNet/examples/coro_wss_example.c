/**
 * @file coro_wss_example.c
 * @brief Demonstration of coroutine-based WebSocket Secure (WSS) client.
 *
 * Connects to a WSS echo server, sends a message, receives the echo.
 */

#include "turbo_coro_socket.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wss_task(coro_t* co, void* arg) {
    UNUSED(co);
    coro_context_t* ctx = (coro_context_t*)arg;

    printf("[Coro] Creating WebSocket client (WSS)...\n");
    coro_socket_t* client = coro_socket_create(ctx, CORO_SOCKET_TLS_V4);

    coro_socket_set_timeout(client, 15000);

    /* Socket type is TLS; coro_socket_connect_ws handles the WS upgrade */
    printf("[Coro] Connecting to wss://echo.websocket.org ...\n");
    int r = coro_socket_connect_ws(client, "echo.websocket.org", 443, "/", 1);

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
    char* data = NULL;
    size_t len = 0;
    r = coro_socket_recv(client, &data, &len);

    if (r == 0 && data) {
        char buf[512];
        size_t to_print = len > sizeof(buf) - 1 ? sizeof(buf) - 1 : len;
        memcpy(buf, data, to_print);
        buf[to_print] = '\0';
        printf("[Coro] Received echo (%zu bytes): %s\n", len, buf);
        coro_socket_free_recv(data);
    } else {
        printf("[Coro] Recv failed: (code %d) %s\n", r, turbo_strerror(r));
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
