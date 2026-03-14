/**
 * @file worker_tlv_example.c
 * @brief REQUEST mode worker — handles TLV messages, stays connected.
 *
 * TLV format: [type:1][len:2 big-endian][payload:len]
 * Worker echoes each message with type high bit set (response marker).
 *
 * Usage: worker_tlv_example [lb_backend_url]
 */

#include "CoroNet/turbo_coro_socket.h"
#include <CoroNet/turbo_coro_context.h>
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_backend_url = "tcp://127.0.0.1:9090";

static void worker_loop(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = (coro_context_t *)arg;

    while (1) {
        coro_socket_t *c = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
        if (!c) { coro_sleep(ctx, 1000); continue; }

        printf("Connecting to %s...\n", g_backend_url);
        if (coro_socket_connect(c, g_backend_url) != 0) {
            printf("Connect failed, retrying in 1s\n");
            coro_socket_destroy(c);
            coro_sleep(ctx, 1000);
            continue;
        }
        printf("Connected, handling TLV messages\n");

        char *data = NULL;
        size_t len = 0;
        int count = 0;

        while (coro_socket_recv(c, &data, &len) == 0) {
            if (len >= 3) {
                uint8_t type = (uint8_t)data[0];
                uint16_t plen =
                    (uint16_t)((unsigned char)data[1] << 8 |
                               (unsigned char)data[2]);
                printf("  msg #%d: type=0x%02X payload=%u bytes\n",
                       ++count, type, plen);

                /* Response: set high bit on type */
                data[0] = (char)(type | 0x80);
            }
            coro_socket_send(c, data, len);
            free(data);
            data = NULL;
        }

        printf("Disconnected after %d messages, reconnecting\n", count);
        coro_socket_destroy(c);
    }
}

int main(int argc, char **argv) {
    if (argc > 1) g_backend_url = argv[1];

    coro_context_t *ctx = coro_context_create(NULL);
    if (!ctx) { fprintf(stderr, "context create failed\n"); return 1; }

    coro_t *co = coro_create(worker_loop, ctx, NULL);
    coro_resume(co);

    printf("TLV worker running, backend=%s\n", g_backend_url);
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    return 0;
}
