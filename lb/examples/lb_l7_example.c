/**
 * @file lb_l7_example.c
 * @brief L7 Load Balancer example — routes by message prefix.
 *
 * Routes "API:..." to "api" worker group, "WEB:..." to "web" group.
 *
 * Usage:
 *   lb_l7_example [frontend_port] [backend_port]
 *   Default: frontend=8080, backend=9090
 *
 * Then run workers:
 *   worker_l7_example api tcp://127.0.0.1:9090
 *   worker_l7_example web tcp://127.0.0.1:9090
 *
 * Test with:
 *   echo "API:hello" | nc 127.0.0.1 8080
 *   echo "WEB:hello" | nc 127.0.0.1 8080
 */

#include "turbo_coro_lb.h"
#include <CoroNet/turbo_coro_context.h>
#include <stdio.h>
#include <string.h>

static const char *route_by_prefix(const char *data, size_t len, void *arg) {
    (void)arg;
    if (len >= 4 && memcmp(data, "API:", 4) == 0) return "api";
    if (len >= 4 && memcmp(data, "WEB:", 4) == 0) return "web";
    printf("  route: unknown prefix, using default group\n");
    return NULL;
}

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

    coro_lb_config_t config = {
        .balance = TURBO_LB_ROUND_ROBIN,
        .route_cb = route_by_prefix,
        .route_cb_arg = NULL,
        .peek_bytes = 4096,
    };

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

    if (coro_lb_accept_workers(lb, backend) != 0) {
        fprintf(stderr, "Failed to accept workers on %s\n", backend);
        coro_lb_destroy(lb);
        coro_context_destroy(ctx);
        return 1;
    }

    printf("L7 LB running\n");
    printf("  frontend: %s\n", frontend);
    printf("  backend:  %s\n", backend);
    printf("  routing:  API:* -> api group, WEB:* -> web group\n");

    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_lb_destroy(lb);
    coro_context_destroy(ctx);
    return 0;
}
