/**
 * @file worker_example.c
 * @brief Worker example — connects to LB backend and echoes data.
 *
 * Usage: worker_example [lb_backend_url]
 *   Default: tcp://127.0.0.1:9090
 *
 * The worker handler is identical to a coro_server handler.
 */

#include <netcore/turbo_coro_client.h>
#include <netcore/turbo_coro_context.h>
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>

static const char *g_backend_url = "tcp://127.0.0.1:9090";

static void echo_handler(coro_client_t *client) {
    char *data = NULL;
    size_t len = 0;
    while (coro_client_recv(client, &data, &len) == 0) {
        coro_client_send(client, data, len);
        free(data);
        data = NULL;
    }
}

static void worker_loop(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = (coro_context_t *)arg;

    while (1) {
        coro_client_t *c = coro_client_create(ctx);
        if (!c) {
            coro_sleep(ctx, 1000);
            continue;
        }

        printf("Worker connecting to %s...\n", g_backend_url);
        if (coro_client_connect(c, g_backend_url) != 0) {
            printf("Worker connect failed, retrying in 1s\n");
            coro_client_destroy(c);
            coro_sleep(ctx, 1000);
            continue;
        }

        printf("Worker connected, serving client\n");
        echo_handler(c);
        printf("Worker session ended, reconnecting\n");

        coro_client_destroy(c);
    }
}

int main(int argc, char **argv) {
    if (argc > 1) g_backend_url = argv[1];

    coro_context_t *ctx = coro_context_create(NULL);
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    coro_t *co = coro_create(worker_loop, ctx, NULL);
    coro_resume(co);

    printf("Worker running, connecting to %s\n", g_backend_url);
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    return 0;
}
