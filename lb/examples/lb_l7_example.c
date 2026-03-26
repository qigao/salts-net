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
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *route_by_prefix(const char *data, size_t len, void *arg) {
    (void)arg;
    if (len >= 4 && memcmp(data, "API:", 4) == 0) return "api";
    if (len >= 4 && memcmp(data, "WEB:", 4) == 0) return "web";
    printf("  route: unknown prefix, using default group\n");
    return NULL;
}

static int parse_tcp_endpoint(const char *endpoint, char *host, size_t host_cap, int *port) {
    const char *start = endpoint;
    const char *colon;
    size_t host_len;

    if (!endpoint || !host || !port || host_cap == 0) return -1;
    if (strncmp(start, "tcp://", 6) == 0) start += 6;
    colon = strrchr(start, ':');
    if (!colon || colon == start || colon[1] == '\0') return -1;

    host_len = (size_t)(colon - start);
    if (host_len >= host_cap) return -1;
    memcpy(host, start, host_len);
    host[host_len] = '\0';
    *port = atoi(colon + 1);
    return (*port > 0) ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *frontend = "tcp://0.0.0.0:8080";
    const char *backend = "tcp://0.0.0.0:9090";
    char frontend_host[64];
    char backend_host[64];
    int frontend_port;
    int backend_port;

    if (argc > 1) {
        static char fbuf[64];
        fmt(fbuf, sizeof(fbuf), "tcp://0.0.0.0:{}", argv[1]);
        frontend = fbuf;
    }
    if (argc > 2) {
        static char bbuf[64];
        fmt(bbuf, sizeof(bbuf), "tcp://0.0.0.0:{}", argv[2]);
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

    if (parse_tcp_endpoint(frontend, frontend_host, sizeof(frontend_host), &frontend_port) != 0) {
        fprintf(stderr, "Invalid frontend endpoint %s\n", frontend);
        coro_lb_destroy(lb);
        coro_context_destroy(ctx);
        return 1;
    }
    if (parse_tcp_endpoint(backend, backend_host, sizeof(backend_host), &backend_port) != 0) {
        fprintf(stderr, "Invalid backend endpoint %s\n", backend);
        coro_lb_destroy(lb);
        coro_context_destroy(ctx);
        return 1;
    }

    if (coro_lb_listen(lb, frontend_host, frontend_port) != 0) {
        fprintf(stderr, "Failed to listen on %s\n", frontend);
        coro_lb_destroy(lb);
        coro_context_destroy(ctx);
        return 1;
    }

    if (coro_lb_accept_workers(lb, backend_host, backend_port) != 0) {
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
