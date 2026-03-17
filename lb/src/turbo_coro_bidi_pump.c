/**
 * @file coro_bidi_pump.c
 * @brief Bidirectional data pump — shared between LB and tproxy.
 */

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

#include "turbo_coro_bidi_pump.h"
#include <turbo_coro.h>
#include <CoroNet/turbo_coro_context.h>
#include "CoroNet/turbo_coro_socket.h"
#include <CoroNet/turbo_coro_internal.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <uv.h>

typedef struct {
    coro_socket_t *src;
    coro_socket_t *dst;
    size_t rate_limit_bps;
    size_t bytes_transferred;
    uint64_t start_time;
    volatile int alive;
    volatile int done;
} pump_dir_ctx_t;

static void pump_reverse_coro(coro_t *co, void *arg) {
    (void)co;
    pump_dir_ctx_t *ctx = (pump_dir_ctx_t *)arg;
    char *data = NULL;
    size_t len = 0;

    while (ctx->alive) {
        int r = coro_socket_recv(ctx->src, &data, &len);

        if (r < 0 || r == TURBO_EOF) {
            break;
        }

        r = coro_socket_send(ctx->dst, data, len);
        if (r < 0) {
            coro_socket_free_recv(data);
            break;
        }

        if (ctx->rate_limit_bps > 0) {
            ctx->bytes_transferred += len;
            uint64_t elapsed_ns = uv_hrtime() - ctx->start_time;
            uint64_t expected_ns = (ctx->bytes_transferred * 1000000000ULL) /
                                   ctx->rate_limit_bps;
            if (expected_ns > elapsed_ns) {
                coro_sleep(coro_socket_get_context(ctx->src),
                                 (expected_ns - elapsed_ns) / 1000000);
            }
        }


        coro_socket_free_recv(data);
        data = NULL;
    }

    ctx->alive = 0;

    /* Symmetrically wake the forward coroutine if it is polling a (ctx->dst) */
    coro_client_wake_eof(ctx->dst);

    ctx->done = 1;
}

void coro_bidi_pump(coro_socket_t *a, coro_socket_t *b,
                           const turbo_bidi_pump_config_t *config) {
    size_t rate = config ? config->rate_limit_bps : 0;

    pump_dir_ctx_t *rev = (pump_dir_ctx_t *)malloc(sizeof(pump_dir_ctx_t));
    if (!rev) return;
    rev->src = b;
    rev->dst = a;
    rev->rate_limit_bps = rate;
    rev->bytes_transferred = 0;
    rev->start_time = uv_hrtime();
    rev->alive = 1;
    rev->done = 0;

    coro_t *co = coro_create(pump_reverse_coro, rev, NULL);
    coro_resume(co);

    char *data = NULL;
    size_t len = 0;

    while (rev->alive) {
        int r = coro_socket_recv(a, &data, &len);

        if (r < 0 || r == TURBO_EOF) {
            break;
        }

        r = coro_socket_send(b, data, len);
        if (r < 0) {
            coro_socket_free_recv(data);
            break;
        }

        if (rate > 0) {
            rev->bytes_transferred += len;
            uint64_t elapsed_ns = uv_hrtime() - rev->start_time;
            uint64_t expected_ns =
                (rev->bytes_transferred * 1000000000ULL) / rate;
            if (expected_ns > elapsed_ns) {
                coro_sleep(coro_socket_get_context(a),
                                 (expected_ns - elapsed_ns) / 1000000);
            }
        }

        coro_socket_free_recv(data);
        data = NULL;
    }

    rev->alive = 0;
    
    coro_client_wake_eof(b);

    /* Wait for reverse coroutine to truly exit before returning. */
    while (!rev->done) {
        coro_sleep(coro_socket_get_context(a), 10);
    }

    coro_destroy(co);
    free(rev);
}
