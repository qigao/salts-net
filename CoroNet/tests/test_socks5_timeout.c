/**
 * @file test_socks5_timeout.c
 * @brief Test SOCKS5 proxy timeout functionality
 */

#include "tinytest.h"
#include "turbo_tcp.h"
#include "turbo_socks5.h"
#include <uv.h>
#include <string.h>

static int g_timeout_triggered = 0;
static uv_loop_t* g_loop = NULL;

/* ── Callbacks ────────────────────────────────────────────────── */

static int on_recv(void* handle, const mem_slice_t* data, void* peer) {
    (void)handle;
    (void)data;
    (void)peer;
    return 0;
}

static void on_connect(turbo_tcp_client_t* client, int status, void* peer) {
    (void)client;
    (void)peer;

    if (status == UV_ETIMEDOUT) {
        printf("Connection timed out as expected\n");
        g_timeout_triggered = 1;
    } else {
        printf("Connection status: %d\n", status);
    }

    uv_stop(g_loop);
}

static void on_close(turbo_tcp_client_t* client) {
    (void)client;
}

static void on_manual_timeout(uv_timer_t* timer) {
    uv_stop(timer->loop);
}

/* ── Tests ────────────────────────────────────────────────────── */

suite("SOCKS5 Timeout") {
    describe("Timeout Control") {
        it("should timeout when proxy is unreachable") {
            g_loop = uv_default_loop();
            g_timeout_triggered = 0;

            turbo_tcp_client_t* client = turbo_tcp_client_create(g_loop);
            check(client != NULL);

            /* Configure proxy with timeout */
            turbo_socks5_config_t proxy = {0};
            strcpy(proxy.host, "192.0.2.1");  /* TEST-NET-1, should be unreachable */
            proxy.port = 1080;
            proxy.auth_required = 0;
            proxy.timeout_ms = 2000;  /* 2 second timeout */

            int rc = turbo_tcp_client_connect_via_proxy(
                client, "example.com", 80, &proxy,
                on_recv, on_connect, on_close
            );
            check_int_eq(rc, 0);

            /* Run event loop */
            uv_run(g_loop, UV_RUN_DEFAULT);

            /* Verify timeout was triggered */
            check_int_eq(g_timeout_triggered, 1);
        }

        it("should not timeout with zero timeout_ms") {
            g_loop = uv_default_loop();
            g_timeout_triggered = 0;

            turbo_tcp_client_t* client = turbo_tcp_client_create(g_loop);
            check(client != NULL);

            /* Configure proxy without timeout */
            turbo_socks5_config_t proxy = {0};
            strcpy(proxy.host, "127.0.0.1");
            proxy.port = 9999;  /* Non-existent proxy */
            proxy.auth_required = 0;
            proxy.timeout_ms = 0;  /* No timeout */

            int rc = turbo_tcp_client_connect_via_proxy(
                client, "example.com", 80, &proxy,
                on_recv, on_connect, on_close
            );
            check_int_eq(rc, 0);

            /* Run with manual timeout */
            uv_timer_t timer;
            uv_timer_init(g_loop, &timer);
            uv_timer_start(&timer, on_manual_timeout, 3000, 0);

            uv_run(g_loop, UV_RUN_DEFAULT);

            /* Should not have timed out via SOCKS5 timeout */
            check_int_eq(g_timeout_triggered, 0);

            turbo_tcp_client_close(client);
            uv_run(g_loop, UV_RUN_DEFAULT);
        }
    }
}
