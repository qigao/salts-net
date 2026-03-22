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

static int s_tls_connected = -1;
static char s_tls_rx_buf[4096];
static size_t s_tls_rx_len = 0;

static void on_tls_connect(void *handle, int status, void *peer) {
    (void)peer;
    (void)handle;
    s_tls_connected = status;
}

static int on_tls_recv(void *handle, const mem_slice_t *slice, void *peer) {
    (void)peer;
    if (slice && slice->data && slice->length > 0) {
        if (s_tls_rx_len + slice->length < sizeof(s_tls_rx_buf)) {
            memcpy(s_tls_rx_buf + s_tls_rx_len, slice->data, slice->length);
            s_tls_rx_len += slice->length;
            s_tls_rx_buf[s_tls_rx_len] = '\0';
        }
    }
    return 0;
}

static void on_tls_close(void *handle) {
    (void)handle;
}

spec("Stream TLS Client") {
    it("should connect, handshake, send and receive encrypted data") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        turbo_stream_t *s = turbo_stream_create(ctx, TURBO_STREAM_TLS);
        check(s != NULL);

        extern void turbo_stream_tls_set_sni(turbo_stream_t *s, const char *hostname);
        turbo_stream_tls_set_sni(s, "www.google.com");

        s_tls_connected = -1;
        s_tls_rx_len = 0;
        memset(s_tls_rx_buf, 0, sizeof(s_tls_rx_buf));

        /* Resolve google manually for now */
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int err = getaddrinfo("www.google.com", "443", &hints, &res);
        check_int_eq(err, 0);

        int r = turbo_stream_connect_addr(s, res->ai_addr, on_tls_connect, on_tls_close);
        freeaddrinfo(res);
        check_int_eq(r, 0);

        int limit = 100000;
        while (s_tls_connected == -1 && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }
        check_int_eq(s_tls_connected, 0);

        turbo_stream_recv_start(s, on_tls_recv);

        const char *req = "GET / HTTP/1.1\r\nHost: www.google.com\r\nConnection: close\r\n\r\n";
        turbo_stream_send(s, req, strlen(req));
        turbo_stream_flush(s);

        limit = 100000;
        while (s_tls_rx_len == 0 && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        check_int_eq(s_tls_connected, 0);
        check(s_tls_rx_len > 0);
        check(strstr(s_tls_rx_buf, "HTTP/1.1") != NULL);

        turbo_stream_close(s);
        
        limit = 1000;
        while (limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        coro_context_destroy(ctx);
    }
}
