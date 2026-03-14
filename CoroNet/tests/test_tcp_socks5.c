/**
 * @file test_tcp_socks5.c
 * @brief Test TCP client with SOCKS5 proxy support
 *
 * USAGE:
 *   1. Start SOCKS5 proxy: ssh -D 1080 user@host
 *   2. Run this test
 */

#include "tinytest.h"
#include "turbo_tcp.h"
#include "turbo_socks5.h"
#include <uv.h>
#include <string.h>

static int g_connected = 0;
static int g_received = 0;
static uv_loop_t* g_loop = NULL;

/* ── Callbacks ────────────────────────────────────────────────── */

static int on_recv(void* handle, const mem_slice_t* data, void* peer) {
    (void)handle;
    (void)peer;
    printf("Received %zu bytes: %.*s\n", data->length, (int)data->length, data->data);
    g_received = 1;
    uv_stop(g_loop);
    return 0;
}

static void on_connect(turbo_tcp_client_t* client, int status, void* peer) {
    (void)peer;
    (void)client;
    if (status == 0) {
        printf("Connected via SOCKS5 proxy!\n");
        g_connected = 1;

        /* Send HTTP request */
        const char* request = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
        turbo_tcp_send(client, request, strlen(request));
    } else {
        printf("Connection failed: %d\n", status);
        g_connected = 0;
        uv_stop(g_loop);
    }
}

static void on_close(turbo_tcp_client_t* client) {
    (void)client;
    printf("Connection closed\n");
}

static void on_timeout(uv_timer_t* timer) {
    uv_stop(timer->loop);
}

/* ── Tests ────────────────────────────────────────────────────── */

suite("TCP SOCKS5 Proxy") {
    describe("SOCKS5 Proxy Connection") {
        it("should connect via proxy (manual test)") {
            /* This test requires:
             * 1. A running SOCKS5 proxy on localhost:1080
             * 2. Internet access to example.com
             *
             * To run: ssh -D 1080 user@host
             */

            info("Requires manual SOCKS5 proxy setup");

            /*
            g_loop = uv_default_loop();
            g_connected = 0;
            g_received = 0;

            // Create client
            turbo_tcp_client_t* client = turbo_tcp_client_create(g_loop);
            check(client != NULL);

            // Configure proxy
            turbo_socks5_config_t proxy = {0};
            strcpy(proxy.host, "127.0.0.1");
            proxy.port = 1080;
            proxy.auth_required = 0;

            // Connect via proxy
            int rc = turbo_tcp_client_connect_via_proxy(
                client, "example.com", 80, &proxy,
                on_recv, on_connect, on_close
            );
            check_int_eq(rc, 0);

            // Run event loop
            uv_run(g_loop, UV_RUN_DEFAULT);

            // Verify connection
            check_int_eq(g_connected, 1);
            check_int_eq(g_received, 1);

            // Cleanup
            turbo_tcp_client_close(client);
            uv_run(g_loop, UV_RUN_DEFAULT);
            */
        }

        it("should handle proxy connection failure") {
            g_loop = uv_default_loop();
            g_connected = 0;

            turbo_tcp_client_t* client = turbo_tcp_client_create(g_loop);
            check(client != NULL);

            /* Try to connect to non-existent proxy */
            turbo_socks5_config_t proxy = {0};
            strcpy(proxy.host, "127.0.0.1");
            proxy.port = 9999;  /* Non-existent proxy */
            proxy.auth_required = 0;

            int rc = turbo_tcp_client_connect_via_proxy(
                client, "example.com", 80, &proxy,
                on_recv, on_connect, on_close
            );
            check_int_eq(rc, 0);  /* Should return 0 (async) */

            /* Run with timeout */
            uv_timer_t timer;
            uv_timer_init(g_loop, &timer);
            uv_timer_start(&timer, on_timeout, 2000, 0);

            uv_run(g_loop, UV_RUN_DEFAULT);

            /* Should not have connected */
            check_int_eq(g_connected, 0);

            turbo_tcp_client_close(client);
            uv_run(g_loop, UV_RUN_DEFAULT);
        }
    }

    describe("API Validation") {
        it("should reject NULL parameters") {
            g_loop = uv_default_loop();
            turbo_tcp_client_t* client = turbo_tcp_client_create(g_loop);
            turbo_socks5_config_t proxy = {0};

            /* NULL client */
            int rc = turbo_tcp_client_connect_via_proxy(
                NULL, "example.com", 80, &proxy,
                on_recv, on_connect, on_close
            );
            check_int_eq(rc, UV_EINVAL);

            /* NULL host */
            rc = turbo_tcp_client_connect_via_proxy(
                client, NULL, 80, &proxy,
                on_recv, on_connect, on_close
            );
            check_int_eq(rc, UV_EINVAL);

            /* NULL proxy */
            rc = turbo_tcp_client_connect_via_proxy(
                client, "example.com", 80, NULL,
                on_recv, on_connect, on_close
            );
            check_int_eq(rc, UV_EINVAL);

            turbo_tcp_client_close(client);
            uv_run(g_loop, UV_RUN_DEFAULT);
        }
    }
}
