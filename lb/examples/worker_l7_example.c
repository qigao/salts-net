/**
 * @file worker_l7_example.c
 * @brief L7 Worker example — registers with a group name, then echoes.
 *
 * Usage:
 *   worker_l7_example <group> [lb_backend_host [lb_backend_port]]
 *   Example: worker_l7_example api 127.0.0.1 9090
 *
 * The worker sends its group name as the first message after connecting,
 * then runs a normal echo handler — identical to a coro_server handler.
 */

#include "CoroNet/turbo_coro_socket.h"
#include <CoroNet/turbo_coro_context.h>
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_backend_host = "127.0.0.1";
static int g_backend_port = 9090;
static const char *g_group = "default";

static void echo_handler(coro_socket_t *client) {
    char *data = NULL;
    size_t len = 0;
    while (coro_socket_recv(client, &data, &len) == 0) {
        /* Echo back with group tag */
        size_t glen = strlen(g_group);
        size_t total = 1 + glen + 1 + len; /* [group]data */
        char *resp = (char *)malloc(total);
        if (resp) {
            resp[0] = '[';
            memcpy(resp + 1, g_group, glen);
            resp[1 + glen] = ']';
            memcpy(resp + 2 + glen, data, len);
            coro_socket_send(client, resp, 2 + glen + len);
            free(resp);
        }
        coro_socket_free_recv(data);
        data = NULL;
    }
}

static void worker_loop(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = (coro_context_t *)arg;

    while (1) {
        coro_socket_t *c = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
        if (!c) {
            coro_sleep(ctx, 1000);
            continue;
        }

        printf("[%s] Connecting to %s:%d...\n", g_group, g_backend_host, g_backend_port);
        if (coro_socket_connect(c, g_backend_host, g_backend_port) != 0) {
            printf("[%s] Connect failed, retrying in 1s\n", g_group);
            coro_socket_destroy(c);
            coro_sleep(ctx, 1000);
            continue;
        }

        /* Register group name */
        coro_socket_send(c, g_group, strlen(g_group));
        printf("[%s] Registered, waiting for client\n", g_group);

        echo_handler(c);
        printf("[%s] Session ended, reconnecting\n", g_group);

        coro_socket_destroy(c);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <group> [lb_backend_host [lb_backend_port]]\n", argv[0]);
        fprintf(stderr, "  group: worker group name (e.g. api, web)\n");
        return 1;
    }

    g_group = argv[1];
    if (argc > 2) g_backend_host = argv[2];
    if (argc > 3) g_backend_port = atoi(argv[3]);

    coro_context_t *ctx = coro_context_create(NULL);
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    if (coro_context_spawn(ctx, worker_loop, ctx) != 0) {
        fprintf(stderr, "Failed to spawn worker loop\n");
        coro_context_destroy(ctx);
        return 1;
    }

    printf("[%s] Worker running, backend=%s:%d\n", g_group, g_backend_host, g_backend_port);
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    return 0;
}
