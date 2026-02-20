/**
 * @file coro_ws_example.c
 * @brief Demonstration of coroutine-based WebSocket client.
 *
 * Connects to a WS echo server, sends a message, receives the echo.
 */

#include "turbo_coro_client.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ws_task(turbo_coro_t* co, void* arg) {
    UNUSED(co);
    turbo_coro_context_t* ctx = (turbo_coro_context_t*)arg;

    printf("[Coro] Creating WebSocket client...\n");
    turbo_coro_client_t* client = turbo_coro_client_create(ctx);

    turbo_coro_client_set_timeout(client, 15000);

    printf("[Coro] Connecting to ws://websockets.chilkat.io ...\n");
    int r = turbo_coro_client_connect(client, "ws://websockets.chilkat.io/wsChilkatEcho.ashx");

    if (r != 0) {
        printf("[Coro] Connection failed: (code %d) %s\n", r, turbo_strerror(r));
        turbo_coro_client_destroy(client);
        return;
    }

    printf("[Coro] Connected! Sending message...\n");
    const char* msg = "Hello from TurboNet coro WebSocket!";
    r = turbo_coro_client_send(client, msg, strlen(msg));

    if (r != 0) {
        printf("[Coro] Send failed: (code %d) %s\n", r, turbo_strerror(r));
        turbo_coro_client_destroy(client);
        return;
    }

    printf("[Coro] Waiting for echo...\n");
    char* data = NULL;
    size_t len = 0;
    r = turbo_coro_client_recv(client, &data, &len);

    if (r == 0 && data) {
        char buf[512];
        size_t to_print = len > sizeof(buf) - 1 ? sizeof(buf) - 1 : len;
        memcpy(buf, data, to_print);
        buf[to_print] = '\0';
        printf("[Coro] Received echo (%zu bytes): %s\n", len, buf);
        free(data);
    } else {
        printf("[Coro] Recv failed: (code %d) %s\n", r, turbo_strerror(r));
    }

    turbo_coro_client_destroy(client);
    printf("[Coro] WebSocket task complete.\n");
}

int main(void) {
    printf("[Main] Initializing context\n");
    turbo_coro_context_t* ctx = turbo_coro_context_create();

    printf("[Main] Creating coroutine\n");
    turbo_coro_t* co = turbo_coro_create(ws_task, ctx, NULL);

    printf("[Main] Starting coroutine...\n");
    turbo_coro_resume(co);

    printf("[Main] Running loop...\n");
    turbo_coro_context_run(ctx);

    turbo_coro_destroy(co);
    turbo_coro_context_destroy(ctx);
    printf("[Main] Done.\n");
    return 0;
}
