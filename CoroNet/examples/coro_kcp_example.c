#include "CoroNet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KCP_PORT 9200
#define KCP_URL "kcp://127.0.0.1:9200"

/* ── Server handler: echo back whatever we receive ────────── */

static void kcp_echo_handler(coro_socket_t* client, void* arg) {
    (void)arg;
    printf("[Server] KCP client connected\n");

    char* data = NULL;
    size_t len = 0;

    while (1) {
        int r = coro_socket_recv(client, &data, &len);
        if (r != 0 || len == 0) {
            if (r != 0) printf("[Server] Recv failed: %s\n", turbo_strerror(r));
            break;
        }
        printf("[Server] Received %zu bytes, echoing back\n", len);
        coro_socket_send(client, data, len);
        coro_socket_free_recv(data);
        data = NULL;
    }

    printf("[Server] Handler done\n");
}

/* ── Client coroutine ─────────────────────────────────────── */

static void kcp_client_task(coro_t* co, void* arg) {
    (void)co;
    coro_context_t* ctx = (coro_context_t*)arg;

    printf("[Client] Connecting to %s...\n", KCP_URL);
    coro_socket_t* client = coro_socket_create_kcp(ctx);
    coro_socket_set_timeout(client, 5000);

    int r = coro_socket_connect(client, KCP_URL);
    if (r != 0) {
        printf("[Client] Connect failed: %s\n", turbo_strerror(r));
        coro_socket_destroy(client);
        return;
    }

    printf("[Client] Connected! Sending message...\n");
    const char* msg = "Hello KCP from coroutine!";
    r = coro_socket_send(client, msg, strlen(msg));
    if (r != 0) {
        printf("[Client] Send failed: %s\n", turbo_strerror(r));
        coro_socket_destroy(client);
        return;
    }

    char* data = NULL;
    size_t len = 0;
    r = coro_socket_recv(client, &data, &len);
    if (r == 0 && data) {
        printf("[Client] Echo received: %.*s\n", (int)len, data);
        coro_socket_free_recv(data);
    } else {
        printf("[Client] Recv failed: %s\n", turbo_strerror(r));
    }

    coro_socket_destroy(client);
    printf("[Client] Done\n");
}

/* ── Launcher: start server, then client after a short delay ─ */

static void launcher_task(coro_t* co, void* arg) {
    (void)co;
    coro_context_t* ctx = (coro_context_t*)arg;

    /* Start server */
    coro_socket_t* server = coro_socket_create_kcp(ctx);
    int r = coro_socket_listen_url(server, KCP_URL, kcp_echo_handler, NULL);
    if (r != 0) {
        printf("[Launcher] Server listen failed: %s\n", turbo_strerror(r));
        coro_socket_destroy(server);
        return;
    }
    printf("[Launcher] KCP server listening on %s\n", KCP_URL);

    /* Give server a moment to be ready */
    coro_sleep(ctx, 100);

    /* Spawn client */
    coro_context_spawn(ctx, kcp_client_task, ctx);

    /* Wait for client to finish */
    coro_sleep(ctx, 2000);

    printf("[Launcher] Shutting down server\n");
    coro_socket_destroy(server);

    /* Wait for handler coroutine to complete */
    coro_sleep(ctx, 200);

    /* Stop event loop */
    coro_context_stop(ctx);
}

int main(void) {
    printf("=== KCP Coroutine Echo Example ===\n");
    printf("[Main] Initializing context\n");
    coro_context_t* ctx = coro_context_create(NULL);

    coro_context_spawn(ctx, launcher_task, ctx);

    printf("[Main] Starting event loop...\n");
   coro_context_run(ctx, TURBO_RUN_DEFAULT);

    /* Drain remaining coroutines */
    int max_drain = 200;
    while (max_drain-- > 0) {
        int has_coros = coro_context_coro_count(ctx);
        if (has_coros == 0) break;
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
    }

    coro_context_destroy(ctx);
    printf("=== Done ===\n");
    return 0;
}
