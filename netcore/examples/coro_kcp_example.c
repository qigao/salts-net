/**
 * @file coro_kcp_example.c
 * @brief KCP echo server + client using coroutines.
 */

#include "turbo_coro_client.h"
#include "turbo_coro_server.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KCP_PORT 9200
#define KCP_URL "kcp://127.0.0.1:9200"

/* ── Server handler: echo back whatever we receive ────────── */

static void kcp_echo_handler(coro_client_t* client, void* arg) {
    (void)arg;
    printf("[Server] KCP client connected\n");

    char* data = NULL;
    size_t len = 0;

    coro_client_set_timeout(client, 5000);

    int r = coro_client_recv(client, &data, &len);
    if (r == 0 && data) {
        printf("[Server] Received %zu bytes, echoing back\n", len);
        coro_client_send(client, data, len);
        free(data);
    } else {
        printf("[Server] Recv failed: %s\n", turbo_strerror(r));
    }

    printf("[Server] Handler done\n");
}

/* ── Client coroutine ─────────────────────────────────────── */

static void kcp_client_task(coro_t* co, void* arg) {
    (void)co;
    coro_context_t* ctx = (coro_context_t*)arg;

    printf("[Client] Connecting to %s...\n", KCP_URL);
    coro_client_t* client = coro_client_create(ctx);
    coro_client_set_timeout(client, 5000);

    int r = coro_client_connect(client, KCP_URL);
    if (r != 0) {
        printf("[Client] Connect failed: %s\n", turbo_strerror(r));
        coro_client_destroy(client);
        return;
    }

    printf("[Client] Connected! Sending message...\n");
    const char* msg = "Hello KCP from coroutine!";
    r = coro_client_send(client, msg, strlen(msg));
    if (r != 0) {
        printf("[Client] Send failed: %s\n", turbo_strerror(r));
        coro_client_destroy(client);
        return;
    }

    char* data = NULL;
    size_t len = 0;
    r = coro_client_recv(client, &data, &len);
    if (r == 0 && data) {
        printf("[Client] Echo received: %.*s\n", (int)len, data);
        free(data);
    } else {
        printf("[Client] Recv failed: %s\n", turbo_strerror(r));
    }

    coro_client_destroy(client);
    printf("[Client] Done\n");
}

/* ── Launcher: start server, then client after a short delay ─ */

static void launcher_task(coro_t* co, void* arg) {
    (void)co;
    coro_context_t* ctx = (coro_context_t*)arg;

    /* Start server */
    coro_server_t* server = coro_server_create(ctx);
    int r = coro_server_listen(server, KCP_URL, kcp_echo_handler, NULL);
    if (r != 0) {
        printf("[Launcher] Server listen failed: %s\n", turbo_strerror(r));
        coro_server_destroy(server);
        return;
    }
    printf("[Launcher] KCP server listening on %s\n", KCP_URL);

    /* Give server a moment to be ready */
    coro_sleep(ctx, 100);

    /* Spawn client */
    coro_context_spawn(ctx, kcp_client_task, ctx);

    /* Wait for client to finish, then tear down */
    coro_sleep(ctx, 8000);

    printf("[Launcher] Shutting down server\n");
    coro_server_destroy(server);
}

int main(void) {
    printf("=== KCP Coroutine Echo Example ===\n");
    printf("[Main] Initializing context\n");
    coro_context_t* ctx = coro_context_create(NULL);

    coro_context_spawn(ctx, launcher_task, ctx);

    printf("[Main] Starting event loop...\n");
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    printf("=== Done ===\n");
    return 0;
}
