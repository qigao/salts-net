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

static void kcp_echo_handler(turbo_coro_client_t* client, void* arg) {
    (void)arg;
    printf("[Server] KCP client connected\n");

    char* data = NULL;
    size_t len = 0;

    turbo_coro_client_set_timeout(client, 5000);

    int r = turbo_coro_client_recv(client, &data, &len);
    if (r == 0 && data) {
        printf("[Server] Received %zu bytes, echoing back\n", len);
        turbo_coro_client_send(client, data, len);
        free(data);
    } else {
        printf("[Server] Recv failed: %s\n", turbo_strerror(r));
    }

    printf("[Server] Handler done\n");
}

/* ── Client coroutine ─────────────────────────────────────── */

static void kcp_client_task(turbo_coro_t* co, void* arg) {
    (void)co;
    turbo_coro_context_t* ctx = (turbo_coro_context_t*)arg;

    printf("[Client] Connecting to %s...\n", KCP_URL);
    turbo_coro_client_t* client = turbo_coro_client_create(ctx);
    turbo_coro_client_set_timeout(client, 5000);

    int r = turbo_coro_client_connect(client, KCP_URL);
    if (r != 0) {
        printf("[Client] Connect failed: %s\n", turbo_strerror(r));
        turbo_coro_client_destroy(client);
        return;
    }

    printf("[Client] Connected! Sending message...\n");
    const char* msg = "Hello KCP from coroutine!";
    r = turbo_coro_client_send(client, msg, strlen(msg));
    if (r != 0) {
        printf("[Client] Send failed: %s\n", turbo_strerror(r));
        turbo_coro_client_destroy(client);
        return;
    }

    char* data = NULL;
    size_t len = 0;
    r = turbo_coro_client_recv(client, &data, &len);
    if (r == 0 && data) {
        printf("[Client] Echo received: %.*s\n", (int)len, data);
        free(data);
    } else {
        printf("[Client] Recv failed: %s\n", turbo_strerror(r));
    }

    turbo_coro_client_destroy(client);
    printf("[Client] Done\n");
}

/* ── Launcher: start server, then client after a short delay ─ */

static void launcher_task(turbo_coro_t* co, void* arg) {
    (void)co;
    turbo_coro_context_t* ctx = (turbo_coro_context_t*)arg;

    /* Start server */
    turbo_coro_server_t* server = turbo_coro_server_create(ctx);
    int r = turbo_coro_server_listen(server, KCP_URL, kcp_echo_handler, NULL);
    if (r != 0) {
        printf("[Launcher] Server listen failed: %s\n", turbo_strerror(r));
        turbo_coro_server_destroy(server);
        return;
    }
    printf("[Launcher] KCP server listening on %s\n", KCP_URL);

    /* Give server a moment to be ready */
    turbo_coro_sleep(ctx, 100);

    /* Spawn client */
    turbo_coro_t* client_co = turbo_coro_create(kcp_client_task, ctx, NULL);
    turbo_coro_resume(client_co);

    /* Wait for client to finish, then tear down */
    turbo_coro_sleep(ctx, 8000);

    printf("[Launcher] Shutting down server\n");
    turbo_coro_server_destroy(server);
}

int main(void) {
    printf("=== KCP Coroutine Echo Example ===\n");
    turbo_coro_context_t* ctx = turbo_coro_context_create();

    turbo_coro_t* co = turbo_coro_create(launcher_task, ctx, NULL);
    turbo_coro_resume(co);

    turbo_coro_context_run(ctx);

    turbo_coro_destroy(co);
    turbo_coro_context_destroy(ctx);
    printf("=== Done ===\n");
    return 0;
}
