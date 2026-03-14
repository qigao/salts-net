/**
 * @file lb_example.c
 * @brief Load Balancer example — starts LB with frontend and backend.
 *
 * Usage: lb_example [frontend_port] [backend_port]
 *   Default: frontend=8080, backend=9090
 */

#include "turbo_coro_lb.h"
#include <CoroNet/turbo_coro_context.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const char *frontend = "tcp://0.0.0.0:8080";
    const char *backend = "tcp://0.0.0.0:9090";

    if (argc > 1) {
        static char fbuf[64];
        snprintf(fbuf, sizeof(fbuf), "tcp://0.0.0.0:%s", argv[1]);
        frontend = fbuf;
    }
    if (argc > 2) {
        static char bbuf[64];
        snprintf(bbuf, sizeof(bbuf), "tcp://0.0.0.0:%s", argv[2]);
        backend = bbuf;
    }

    coro_context_t *ctx = coro_context_create(NULL);
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    coro_lb_config_t config = coro_LB_CONFIG_DEFAULT;
    coro_lb_t *lb = coro_lb_create(ctx, &config);
    if (!lb) {
        fprintf(stderr, "Failed to create LB\n");
        coro_context_destroy(ctx);
        return 1;
    }

    if (coro_lb_listen(lb, frontend) != 0) {
        fprintf(stderr, "Failed to listen on %s\n", frontend);
        coro_lb_destroy(lb);
        coro_context_destroy(ctx);
        return 1;
    }
    printf("LB frontend listening on %s\n", frontend);

    if (coro_lb_accept_workers(lb, backend) != 0) {
        fprintf(stderr, "Failed to accept workers on %s\n", backend);
        coro_lb_destroy(lb);
        coro_context_destroy(ctx);
        return 1;
    }
    printf("LB backend listening on %s\n", backend);

    printf("LB running. Workers connect to backend, clients to frontend.\n");
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_lb_destroy(lb);
    coro_context_destroy(ctx);
    return 0;
}
