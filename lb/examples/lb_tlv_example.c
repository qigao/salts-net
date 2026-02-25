/**
 * @file lb_tlv_example.c
 * @brief REQUEST mode LB with TLV framing and filter.
 *
 * TLV format: [type:1][len:2 big-endian][payload:len]
 *
 * Filter: rejects type=0xFF, drops type=0xFE, accepts everything else.
 * Workers stay connected and handle multiple messages.
 *
 * Usage: lb_tlv_example [frontend_port] [backend_port]
 */

#include "turbo_coro_lb.h"
#include <netcore/turbo_coro_context.h>
#include <stdio.h>
#include <string.h>

static ssize_t tlv_frame_cb(const char *data, size_t len, void *arg) {
    (void)arg;
    if (len < 3) return 0;
    uint16_t payload_len =
        (uint16_t)((unsigned char)data[1] << 8 | (unsigned char)data[2]);
    size_t total = 3 + payload_len;
    return len >= total ? (ssize_t)total : 0;
}

static turbo_lb_filter_result_t tlv_filter(const char *data, size_t len,
                                            void *arg) {
    (void)arg;
    turbo_lb_filter_result_t r = { .verdict = TURBO_LB_ACCEPT };

    if (len >= 1) {
        uint8_t type = (uint8_t)data[0];
        if (type == 0xFF) {
            printf("  filter: REJECT type=0xFF\n");
            static const char err[] = "\xFF\x00\x08" "rejected";
            r.verdict = TURBO_LB_REJECT;
            r.reject_data = err;
            r.reject_len = 11;
        } else if (type == 0xFE) {
            printf("  filter: DROP type=0xFE\n");
            r.verdict = TURBO_LB_DROP;
        } else {
            printf("  filter: ACCEPT type=0x%02X\n", type);
        }
    }
    return r;
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

    turbo_coro_context_t *ctx = turbo_coro_context_create(NULL);
    if (!ctx) { fprintf(stderr, "context create failed\n"); return 1; }

    turbo_coro_lb_config_t config = {
        .balance = TURBO_LB_ROUND_ROBIN,
        .mode = TURBO_LB_MODE_REQUEST,
        .frame_cb = tlv_frame_cb,
        .filter_cb = tlv_filter,
    };

    turbo_coro_lb_t *lb = turbo_coro_lb_create(ctx, &config);
    if (!lb) { fprintf(stderr, "lb create failed\n"); return 1; }

    if (turbo_coro_lb_listen(lb, frontend) != 0) {
        fprintf(stderr, "listen failed: %s\n", frontend);
        return 1;
    }
    if (turbo_coro_lb_accept_workers(lb, backend) != 0) {
        fprintf(stderr, "accept_workers failed: %s\n", backend);
        return 1;
    }

    printf("TLV LB running (REQUEST mode)\n");
    printf("  frontend: %s\n", frontend);
    printf("  backend:  %s\n", backend);
    printf("  filter:   0xFF=reject, 0xFE=drop, else=accept\n");

    turbo_coro_context_run(ctx, TURBO_RUN_DEFAULT);

    turbo_coro_lb_destroy(lb);
    turbo_coro_context_destroy(ctx);
    return 0;
}
