#include "CoroNet.h"
#include "CoroNet/turbo_coro_internal.h"
#include "turbo_stream.h"
#include "tinytest.h"
#include <stdio.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

static int s_connected = -1;
static int s_closed = 0;
static int s_connect_count = 0;

static void on_connect(turbo_stream_t *s, int status, void *arg) {
    (void)arg;
    s_connected = status;
}

static void on_connect_count(turbo_stream_t *s, int status, void *arg) {
    int *status_out = (int *)turbo_stream_get_user_data(s);
    (void)arg;
    if (status_out) {
        *status_out = status;
    }
    if (status == 0) {
        s_connect_count++;
    }
}

static void on_close(turbo_stream_t *s) {
    (void)s;
    s_closed = 1;
}

static turbo_stream_t *s_accepted_client = NULL;
static int s_accepted_count = 0;
static turbo_stream_t *s_accepted_clients[32];

#define STREAM_TEST_WAIT_ITERS 20000

static int stream_test_run_until(coro_context_t *ctx, int *predicate, int expected, int max_iters) {
    int limit = max_iters;

    while (*predicate != expected && limit-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    return *predicate == expected ? 0 : -1;
}

static void on_accept_local(void *server, void *client, void *peer) {
    (void)server; (void)peer;
    s_accepted_client = (turbo_stream_t *)client;
    if (s_accepted_count < (int)(sizeof(s_accepted_clients) / sizeof(s_accepted_clients[0]))) {
        s_accepted_clients[s_accepted_count] = (turbo_stream_t *)client;
    }
    s_accepted_count++;
}

spec("Stream") {
    it("should create and destroy stream") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        turbo_stream_t *stream = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
        check(stream != NULL);

        turbo_stream_destroy(stream);
        coro_context_destroy(ctx);
    }

    it("should fail to connect to missing server and close") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        turbo_stream_t *stream = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
        check(stream != NULL);

        s_connected = -1;
        s_closed = 0;

        int r = turbo_stream_connect(stream, "127.0.0.1", 49200, on_connect, on_close);
        check_int_eq(r, 0); // Submission success

        // Connection might hang for 21 seconds due to Windows Firewall silent dropping on closed ports.
        // We explicitly close it to trigger CancelIo, ensuring an immediate ERROR_OPERATION_ABORTED completion.
        turbo_stream_close(stream);
        turbo_stream_destroy(stream);

        int limit = STREAM_TEST_WAIT_ITERS;
        while (s_connected == -1 && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        check(s_connected != 0); // Should fail to connect (connection refused)
        check(s_connected != -1); // Callback must have fired
        check_int_eq(stream_test_run_until(ctx, &s_closed, 1, STREAM_TEST_WAIT_ITERS), 0);
        check_int_eq(s_closed, 1);
        coro_context_run(ctx, TURBO_RUN_DEFAULT);

        coro_context_destroy(ctx);
    }

#if defined(__linux__) || defined(__ANDROID__)
    it("should reject unavailable io_uring tcp backend") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        check_int_eq(coro_context_set_tcp_backend(ctx, TURBO_TCP_BACKEND_IO_URING),
                     TURBO_ENOTSUP);

        coro_context_destroy(ctx);
    }
#endif

#if defined(__linux__) && defined(TURBO_HAS_IO_URING)
    it("should default tcp sockets to io_uring on linux") {
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *sock;

        check(ctx != NULL);
        sock = coro_socket_create_tcpv4(ctx);
        check(sock != NULL);
        check_int_eq(coro_socket_get_tcp_backend(sock), TURBO_TCP_BACKEND_IO_URING);

        coro_socket_destroy(sock);
        coro_context_destroy(ctx);
    }
#endif

    it("should listen and accept connections") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(49201); // Use a fixed port for test

        s_accepted_client = NULL;
        s_accepted_count = 0;

        turbo_stream_listener_t *listener = turbo_stream_listen(ctx, TURBO_STREAM_TCP4, (struct sockaddr *)&addr, 128, on_accept_local);
        check(listener != NULL);

        /* Connect a client */
        turbo_stream_t *client = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
        check(client != NULL);

        s_connected = -1;
        int r = turbo_stream_connect_addr(client, (struct sockaddr *)&addr, on_connect, on_close);
        check_int_eq(r, 0);

        /* Run loop until connected and accepted */
        int limit = STREAM_TEST_WAIT_ITERS;
        while ((s_connected == -1 || s_accepted_count == 0) && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        check_int_eq(s_connected, 0);
        check_int_eq(s_accepted_count, 1);
        check(s_accepted_client != NULL);

        /* Cleanup */
        turbo_stream_destroy(client);
        if (s_accepted_client) turbo_stream_destroy(s_accepted_client);
        turbo_stream_listener_close(listener);

        coro_context_run(ctx, TURBO_RUN_DEFAULT);
        coro_context_destroy(ctx);
    }

    it("should record listener creation errors on the context") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        check(turbo_stream_listen(ctx, TURBO_STREAM_TCP4, NULL, 128, on_accept_local) == NULL);
        check_int_eq(coro_context_get_last_error(ctx), TURBO_EINVAL);

        check(turbo_stream_create(ctx, (turbo_stream_kind_t)-1) == NULL);
        check_int_eq(coro_context_get_last_error(ctx), TURBO_EPROTONOSUPPORT);

        check(coro_socket_create(ctx, (coro_socket_type_t)-1) == NULL);
        check_int_eq(coro_context_get_last_error(ctx), TURBO_EPROTONOSUPPORT);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);

        turbo_stream_listener_t *listener =
            turbo_stream_listen(ctx, TURBO_STREAM_TCP4, (struct sockaddr *)&addr, 128, on_accept_local);
        check(listener != NULL);
        check_int_eq(coro_context_get_last_error(ctx), 0);

        turbo_stream_listener_close(listener);
        coro_context_run(ctx, TURBO_RUN_DEFAULT);
        coro_context_destroy(ctx);
    }

    it("should propagate stream factory errors through tcp socket connect") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);
        ctx->tcp_backend = (turbo_tcp_backend_t)-1;

        coro_socket_t *sock = coro_socket_create_tcpv4(ctx);
        check(sock != NULL);

        check_int_eq(coro_socket_connect(sock, "127.0.0.1", 49200), TURBO_EPROTONOSUPPORT);
        check_int_eq(coro_context_get_last_error(ctx), TURBO_EPROTONOSUPPORT);

        coro_socket_destroy(sock);
        coro_context_destroy(ctx);
    }

    it("should reject invalid socket helper arguments without crashing") {
        check_int_eq(coro_socket_connect_pipe(NULL, "\\\\.\\pipe\\missing"), TURBO_EINVAL);
        check_int_eq(coro_socket_connect_ws(NULL, "127.0.0.1", 80, "/", 0), TURBO_EINVAL);
        check_int_eq(coro_socket_send(NULL, "x", 1), TURBO_EINVAL);
        check(coro_socket_get_send_buffer(NULL, 16) == NULL);
        check_int_eq(coro_socket_send_buffer(NULL, NULL, 0), TURBO_EINVAL);
        check_int_eq(coro_socket_recv(NULL, NULL, NULL), TURBO_EINVAL);
        check_int_eq(coro_socket_bind(NULL, NULL), TURBO_EINVAL);
        check_int_eq(coro_socket_listen(NULL, 1), TURBO_EINVAL);
        check_int_eq(coro_socket_accept(NULL, NULL), TURBO_EINVAL);
    }

    it("should listen and accept ipv6 connections") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        inet_pton(AF_INET6, "::1", &addr6.sin6_addr);
        addr6.sin6_port = htons(49202);

        memset(s_accepted_clients, 0, sizeof(s_accepted_clients));
        s_accepted_client = NULL;
        s_accepted_count = 0;

        turbo_stream_listener_t *listener =
            turbo_stream_listen(ctx, TURBO_STREAM_TCP6, (struct sockaddr *)&addr6, 128, on_accept_local);
        check(listener != NULL);

        turbo_stream_t *client = turbo_stream_create(ctx, TURBO_STREAM_TCP6);
        check(client != NULL);

        s_connected = -1;
        check_int_eq(turbo_stream_connect_addr(client, (struct sockaddr *)&addr6, on_connect, on_close), 0);

        int limit = STREAM_TEST_WAIT_ITERS;
        while ((s_connected == -1 || s_accepted_count == 0) && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        check_int_eq(s_connected, 0);
        check_int_eq(s_accepted_count, 1);
        check(s_accepted_client != NULL);

        turbo_stream_destroy(client);
        if (s_accepted_client) turbo_stream_destroy(s_accepted_client);
        turbo_stream_listener_close(listener);

        coro_context_run(ctx, TURBO_RUN_DEFAULT);
        coro_context_destroy(ctx);
    }

    it("should accept a burst of client connections on one listener") {
        enum { CLIENT_COUNT = 8 };
        coro_context_t *ctx = coro_context_create(NULL);
        turbo_stream_t *clients[CLIENT_COUNT] = {0};
        int connect_status[CLIENT_COUNT];
        struct sockaddr_in addr;
        int i;
        int limit;

        check(ctx != NULL);

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(49203);

        memset(s_accepted_clients, 0, sizeof(s_accepted_clients));
        memset(connect_status, 0xFF, sizeof(connect_status));
        s_accepted_client = NULL;
        s_accepted_count = 0;
        s_connect_count = 0;

        turbo_stream_listener_t *listener =
            turbo_stream_listen(ctx, TURBO_STREAM_TCP4, (struct sockaddr *)&addr, 128, on_accept_local);
        check(listener != NULL);

        for (i = 0; i < CLIENT_COUNT; i++) {
            clients[i] = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
            check(clients[i] != NULL);
            turbo_stream_set_user_data(clients[i], &connect_status[i]);
            check_int_eq(turbo_stream_connect_addr(clients[i], (struct sockaddr *)&addr,
                                                   on_connect_count, on_close), 0);
        }

        limit = 400;
        while ((s_connect_count < CLIENT_COUNT || s_accepted_count < CLIENT_COUNT) && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        check_int_eq(s_connect_count, CLIENT_COUNT);
        check_int_eq(s_accepted_count, CLIENT_COUNT);
        for (i = 0; i < CLIENT_COUNT; i++) {
            check_int_eq(connect_status[i], 0);
        }

        for (i = 0; i < CLIENT_COUNT; i++) {
            if (clients[i]) turbo_stream_destroy(clients[i]);
            if (i < s_accepted_count && s_accepted_clients[i]) {
                turbo_stream_destroy(s_accepted_clients[i]);
            }
        }
        turbo_stream_listener_close(listener);

        coro_context_run(ctx, TURBO_RUN_DEFAULT);
        coro_context_destroy(ctx);
    }
}
