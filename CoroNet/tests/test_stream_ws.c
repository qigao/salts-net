#include "CoroNet.h"
#include "CoroNet/turbo_stream.h"
#include "tinytest.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#endif

static int s_ws_connected = -1;
static char s_ws_rx_buf[4096];
static size_t s_ws_rx_len = 0;

static void on_ws_connect(void *handle, int status, void *peer) {
    (void)peer;
    (void)handle;
    s_ws_connected = status;
}

static int on_ws_recv(void *handle, const mem_slice_t *slice, void *peer) {
    (void)peer;
    if (slice && slice->data && slice->length > 0) {
        if (s_ws_rx_len + slice->length < sizeof(s_ws_rx_buf)) {
            memcpy(s_ws_rx_buf + s_ws_rx_len, slice->data, slice->length);
            s_ws_rx_len += slice->length;
            s_ws_rx_buf[s_ws_rx_len] = '\0';
        }
    }
    return 0;
}

static void on_ws_close(void *handle) {
    (void)handle;
}

spec("Stream WebSocket Client") {
    it("should connect via WSS, perform handshake and echo data") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        turbo_stream_t *s = turbo_stream_create(ctx, TURBO_STREAM_WSS);
        check(s != NULL);

        /* echo.websocket.org is a standard WS test server. */
        turbo_stream_ws_set_path_host(s, "/", "echo.websocket.org");

        s_ws_connected = -1;
        s_ws_rx_len = 0;
        memset(s_ws_rx_buf, 0, sizeof(s_ws_rx_buf));

        /* Resolve hostname manually for Phase 1 */
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int err = getaddrinfo("echo.websocket.org", "443", &hints, &res);
        check_int_eq(err, 0);

        int r = turbo_stream_connect_addr(s, res->ai_addr, on_ws_connect, on_ws_close);
        freeaddrinfo(res);
        check_int_eq(r, 0);

        int limit = 100000;
        while (s_ws_connected == -1 && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }
        check_int_eq(s_ws_connected, 0);

        turbo_stream_recv_start(s, on_ws_recv);

        const char *req = "Hello WebSocket!";
        turbo_stream_send(s, req, strlen(req));
        turbo_stream_flush(s);

        limit = 100000;
        while (strstr(s_ws_rx_buf, "Hello WebSocket!") == NULL && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        check_int_eq(s_ws_connected, 0);
        check(s_ws_rx_len > 0);
        check(strstr(s_ws_rx_buf, "Hello WebSocket!") != NULL);

        turbo_stream_close(s);
        
        limit = 1000;
        while (limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        coro_context_destroy(ctx);
    }
}