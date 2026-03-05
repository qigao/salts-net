/**
 * @file coro_udp_example.c
 * @brief UDP sendto/recvfrom echo using coroutines.
 */

#include "turbo_coro_client.h"
#include "turbo_coro_server.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UDP_PORT 9300
#define UDP_SERVER_URL "udp://127.0.0.1:9300"
#define UDP_CLIENT_URL "udp://127.0.0.1:0"

/* ── Server: echo datagrams back to sender ────────────────── */

static void udp_echo_handler(coro_client_t* client, void* arg) {
    (void)arg;
    char* data = NULL;
    size_t len = 0;
    int r = coro_client_recv(client, &data, &len);
    if (r == 0 && data) {
        printf("[Server] Received %zu bytes, echoing back\n", len);
        coro_client_send(client, data, len);
    }
}

/* ── Client coroutine: sendto + recvfrom ──────────────────── */

static void udp_client_task(coro_t* co, void* arg) {
    (void)co;
    coro_context_t* ctx = (coro_context_t*)arg;

    coro_client_t* client = coro_client_create(ctx);
    coro_client_set_timeout(client, 5000);

    /* "Connect" just binds locally and sets default peer */
    printf("[Client] Connecting to %s...\n", UDP_SERVER_URL);
    int r = coro_client_connect(client, UDP_SERVER_URL);
    if (r != 0) {
        printf("[Client] Connect failed: %s\n", turbo_strerror(r));
        coro_client_destroy(client);
        return;
    }

    /* Send via connected send (default peer) */
    const char* msg = "Hello UDP from coroutine!";
    printf("[Client] Sending: %s\n", msg);
    r = coro_client_send(client, msg, strlen(msg));
    if (r != 0) {
        printf("[Client] Send failed: %d\n", r);
        coro_client_destroy(client);
        return;
    }

    /* Receive with sender address */
    char* data = NULL;
    size_t len = 0;
    struct sockaddr_storage from;
    r = coro_client_recvfrom(client, &data, &len, &from);
    if (r == 0 && data) {
        printf("[Client] Echo received: %.*s\n", (int)len, data);
        free(data);
    } else {
        printf("[Client] Recv failed: %d\n", r);
    }

    coro_client_destroy(client);
    printf("[Client] Done\n");
}

/* ── Launcher ─────────────────────────────────────────────── */

static void launcher_task(coro_t* co, void* arg) {
    (void)co;
    coro_context_t* ctx = (coro_context_t*)arg;

    /* Start UDP echo server */
    coro_server_t* server = coro_server_create(ctx);
    int r = coro_server_listen(server, UDP_SERVER_URL, udp_echo_handler, NULL);
    if (r != 0) {
        printf("[Launcher] Server listen failed: %d\n", r);
        coro_server_destroy(server);
        return;
    }
    printf("[Launcher] UDP server listening on %s\n", UDP_SERVER_URL);

    /* Give server a moment */
    coro_sleep(ctx, 50);

    /* Spawn client */
    coro_context_spawn(ctx, udp_client_task, ctx);

    /* Wait then tear down */
    coro_sleep(ctx, 8000);

    printf("[Launcher] Shutting down server\n");
    coro_server_destroy(server);
}

int main(void) {
    printf("=== UDP Coroutine Echo Example ===\n");
    printf("[Main] Initializing context\n");
    coro_context_t* ctx = coro_context_create(NULL);

    coro_context_spawn(ctx, launcher_task, ctx);

    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    printf("=== Done ===\n");
    return 0;
}
