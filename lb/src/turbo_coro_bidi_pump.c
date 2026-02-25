/**
 * @file turbo_coro_bidi_pump.c
 * @brief Bidirectional data pump — shared between LB and tproxy.
 */

#include "turbo_coro_bidi_pump.h"
#include "turbo_coro.h"
#include <netcore/turbo_coro_context.h>
#include <stdlib.h>
#include <uv.h>

typedef struct {
    turbo_coro_client_t *src;
    turbo_coro_client_t *dst;
    size_t rate_limit_bps;
    size_t *bytes_transferred;
    uint64_t *start_time;
    int *alive;
} pump_dir_ctx_t;

static void pump_reverse_coro(turbo_coro_t *co, void *arg) {
    (void)co;
    pump_dir_ctx_t *ctx = (pump_dir_ctx_t *)arg;
    char *data = NULL;
    size_t len = 0;

    while (*ctx->alive) {
        int r = turbo_coro_client_recv(ctx->src, &data, &len);
        if (r < 0 || r == TURBO_EOF) break;

        r = turbo_coro_client_send(ctx->dst, data, len);

        if (ctx->rate_limit_bps > 0 && ctx->bytes_transferred &&
            ctx->start_time) {
            *ctx->bytes_transferred += len;
            uint64_t elapsed_ns = uv_hrtime() - *ctx->start_time;
            uint64_t expected_ns = (*ctx->bytes_transferred * 1000000000ULL) /
                                   ctx->rate_limit_bps;
            if (expected_ns > elapsed_ns) {
                turbo_coro_sleep(turbo_coro_client_get_context(ctx->src),
                                 (expected_ns - elapsed_ns) / 1000000);
            }
        }

        free(data);
        data = NULL;
        if (r < 0) break;
    }

    *ctx->alive = 0;
    free(ctx);
}

void turbo_coro_bidi_pump(turbo_coro_client_t *a, turbo_coro_client_t *b,
                           const turbo_bidi_pump_config_t *config) {
    int alive = 1;
    size_t bytes_transferred = 0;
    uint64_t start_time = uv_hrtime();
    size_t rate = config ? config->rate_limit_bps : 0;

    pump_dir_ctx_t *rev = (pump_dir_ctx_t *)malloc(sizeof(pump_dir_ctx_t));
    if (!rev) return;
    rev->src = b;
    rev->dst = a;
    rev->rate_limit_bps = rate;
    rev->bytes_transferred = &bytes_transferred;
    rev->start_time = &start_time;
    rev->alive = &alive;

    turbo_coro_t *co = turbo_coro_create(pump_reverse_coro, rev, NULL);
    turbo_coro_resume(co);

    char *data = NULL;
    size_t len = 0;

    while (alive) {
        turbo_coro_client_set_timeout(a, 1000);
        int r = turbo_coro_client_recv(a, &data, &len);

        if (r == TURBO_ETIMEDOUT) continue;
        if (r < 0 || r == TURBO_EOF) break;

        r = turbo_coro_client_send(b, data, len);

        if (rate > 0) {
            bytes_transferred += len;
            uint64_t elapsed_ns = uv_hrtime() - start_time;
            uint64_t expected_ns =
                (bytes_transferred * 1000000000ULL) / rate;
            if (expected_ns > elapsed_ns) {
                turbo_coro_sleep(turbo_coro_client_get_context(a),
                                 (expected_ns - elapsed_ns) / 1000000);
            }
        }

        free(data);
        data = NULL;
        if (r < 0) break;
    }

    alive = 0;

    /* Let reverse coroutine wake up and exit */
    turbo_coro_sleep(turbo_coro_client_get_context(a), 0);
}
