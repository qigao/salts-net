/**
 * @file worker_l7_example.c
 * @brief L7 Worker example — registers with a group name, then echoes.
 *
 * Usage:
 *   worker_l7_example <group> [lb_backend_url]
 *   Example: worker_l7_example api tcp://127.0.0.1:9090
 *
 * The worker sends its group name as the first message after connecting,
 * then runs a normal echo handler — identical to a turbo_coro_server handler.
 */

#include <netcore/turbo_coro_client.h>
#include <netcore/turbo_coro_context.h>
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_backend_url = "tcp://127.0.0.1:9090";
static const char *g_group = "default";

static void echo_handler(turbo_coro_client_t *client) {
    char *data = NULL;
    size_t len = 0;
    while (turbo_coro_client_recv(client, &data, &len) == 0) {
        /* Echo back with group tag */
        size_t glen = strlen(g_group);
        size_t total = 1 + glen + 1 + len; /* [group]data */
        char *resp = (char *)malloc(total);
        if (resp) {
            resp[0] = '[';
            memcpy(resp + 1, g_group, glen);
            resp[1 + glen] = ']';
            memcpy(resp + 2 + glen, data, len);
            turbo_coro_client_send(client, resp, 2 + glen + len);
            free(resp);
        }
        free(data);
        data = NULL;
    }
}

static void worker_loop(turbo_coro_t *co, void *arg) {
    (void)co;
    turbo_coro_context_t *ctx = (turbo_coro_context_t *)arg;

    while (1) {
        turbo_coro_client_t *c = turbo_coro_client_create(ctx);
        if (!c) {
            turbo_coro_sleep(ctx, 1000);
            continue;
        }

        printf("[%s] Connecting to %s...\n", g_group, g_backend_url);
        if (turbo_coro_client_connect(c, g_backend_url) != 0) {
            printf("[%s] Connect failed, retrying in 1s\n", g_group);
            turbo_coro_client_destroy(c);
            turbo_coro_sleep(ctx, 1000);
            continue;
        }

        /* Register group name */
        turbo_coro_client_send(c, g_group, strlen(g_group));
        printf("[%s] Registered, waiting for client\n", g_group);

        echo_handler(c);
        printf("[%s] Session ended, reconnecting\n", g_group);

        turbo_coro_client_destroy(c);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <group> [lb_backend_url]\n", argv[0]);
        fprintf(stderr, "  group: worker group name (e.g. api, web)\n");
        return 1;
    }

    g_group = argv[1];
    if (argc > 2) g_backend_url = argv[2];

    turbo_coro_context_t *ctx = turbo_coro_context_create(NULL);
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    turbo_coro_t *co = turbo_coro_create(worker_loop, ctx, NULL);
    turbo_coro_resume(co);

    printf("[%s] Worker running, backend=%s\n", g_group, g_backend_url);
    turbo_coro_context_run(ctx, TURBO_RUN_DEFAULT);

    turbo_coro_context_destroy(ctx);
    return 0;
}
