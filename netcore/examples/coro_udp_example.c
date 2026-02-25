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

static void udp_echo_handler(turbo_coro_client_t* client, void* arg) {
    (void)arg;
    char* data = NULL;
    size_t len = 0;
    int r = turbo_coro_client_recv(client, &data, &len);
    if (r == 0 && data) {
        printf("[Server] Received %zu bytes, echoing back\n", len);
        turbo_coro_client_send(client, data, len);
    }
}

/* ── Client coroutine: sendto + recvfrom ──────────────────── */

static void udp_client_task(turbo_coro_t* co, void* arg) {
    (void)co;
    turbo_coro_context_t* ctx = (turbo_coro_context_t*)arg;

    turbo_coro_client_t* client = turbo_coro_client_create(ctx);
    turbo_coro_client_set_timeout(client, 5000);

    /* "Connect" just binds locally and sets default peer */
    printf("[Client] Connecting to %s...\n", UDP_SERVER_URL);
    int r = turbo_coro_client_connect(client, UDP_SERVER_URL);
    if (r != 0) {
        printf("[Client] Connect failed: %s\n", turbo_strerror(r));
        turbo_coro_client_destroy(client);
        return;
    }

    /* Send via connected send (default peer) */
    const char* msg = "Hello UDP from coroutine!";
    printf("[Client] Sending: %s\n", msg);
    r = turbo_coro_client_send(client, msg, strlen(msg));
    if (r != 0) {
        printf("[Client] Send failed: %d\n", r);
        turbo_coro_client_destroy(client);
        return;
    }

    /* Receive with sender address */
    char* data = NULL;
    size_t len = 0;
    struct sockaddr_storage from;
    r = turbo_coro_client_recvfrom(client, &data, &len, &from);
    if (r == 0 && data) {
        printf("[Client] Echo received: %.*s\n", (int)len, data);
        free(data);
    } else {
        printf("[Client] Recv failed: %d\n", r);
    }

    turbo_coro_client_destroy(client);
    printf("[Client] Done\n");
}

/* ── Launcher ─────────────────────────────────────────────── */

static void launcher_task(turbo_coro_t* co, void* arg) {
    (void)co;
    turbo_coro_context_t* ctx = (turbo_coro_context_t*)arg;

    /* Start UDP echo server */
    turbo_coro_server_t* server = turbo_coro_server_create(ctx);
    int r = turbo_coro_server_listen(server, UDP_SERVER_URL, udp_echo_handler, NULL);
    if (r != 0) {
        printf("[Launcher] Server listen failed: %d\n", r);
        turbo_coro_server_destroy(server);
        return;
    }
    printf("[Launcher] UDP server listening on %s\n", UDP_SERVER_URL);

    /* Give server a moment */
    turbo_coro_sleep(ctx, 50);

    /* Spawn client */
    turbo_coro_t* client_co = turbo_coro_create(udp_client_task, ctx, NULL);
    turbo_coro_resume(client_co);

    /* Wait then tear down */
    turbo_coro_sleep(ctx, 8000);

    printf("[Launcher] Shutting down server\n");
    turbo_coro_server_destroy(server);
}

int main(void) {
    printf("=== UDP Coroutine Echo Example ===\n");
    printf("[Main] Initializing context\n");
    turbo_coro_context_t* ctx = turbo_coro_context_create(NULL);

    turbo_coro_t* co = turbo_coro_create(launcher_task, ctx, NULL);
    turbo_coro_resume(co);

    turbo_coro_context_run(ctx, TURBO_RUN_DEFAULT);

    turbo_coro_destroy(co);
    turbo_coro_context_destroy(ctx);
    printf("=== Done ===\n");
    return 0;
}
