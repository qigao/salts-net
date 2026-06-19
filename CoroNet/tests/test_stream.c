#include "CoroNet.h"
#include "CoroNet/turbo_coro_internal.h"
#include "turbo_stream.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static int s_connected = -1;
static int s_closed = 0;
static int s_connect_count = 0;
#ifdef _WIN32
static char s_send_payload[256 * 1024];
#endif

static void on_connect(void *handle, int status, void *arg) {
    (void)handle;
    (void)arg;
    s_connected = status;
}

static void on_connect_count(void *handle, int status, void *arg) {
    turbo_stream_t *s = (turbo_stream_t *)handle;
    int *status_out = (int *)turbo_stream_get_user_data(s);
    (void)arg;
    if (status_out) {
        *status_out = status;
    }
    if (status == 0) {
        s_connect_count++;
    }
}

static void on_close(void *handle) {
    (void)handle;
    s_closed = 1;
}

#ifdef _WIN32
static int on_recv_noop(void *handle, const mem_slice_t *slice, void *arg) {
    (void)handle;
    (void)slice;
    (void)arg;
    return 0;
}
#endif

static int on_recv_capture(void *handle, const mem_slice_t *slice, void *arg);

static turbo_stream_t *s_accepted_client = NULL;
static int s_accepted_count = 0;
static turbo_stream_t *s_accepted_clients[32];
static int s_recv_hit = 0;
static char s_recv_data[64];
static size_t s_recv_len = 0;

static int on_recv_capture(void *handle, const mem_slice_t *slice, void *arg) {
    size_t copy_len;
    (void)handle;
    (void)arg;

    if (!slice || !slice->data) {
        return 0;
    }

    copy_len = slice->length;
    if (copy_len >= sizeof(s_recv_data)) {
        copy_len = sizeof(s_recv_data) - 1U;
    }
    memcpy(s_recv_data, slice->data, copy_len);
    s_recv_data[copy_len] = '\0';
    s_recv_len = copy_len;
    s_recv_hit++;
    return 0;
}

#define STREAM_TEST_WAIT_ITERS 20000

static unsigned short stream_test_pick_loopback_port(void);

static int stream_test_run_until(coro_context_t *ctx, int *predicate, int expected,
                                 uint64_t timeout_ms) {
    uint64_t deadline;

    if (!ctx || !predicate) {
        return -1;
    }

    deadline = turbo_monotonic_ms() + timeout_ms;
    while (*predicate != expected && turbo_monotonic_ms() < deadline) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    return *predicate == expected ? 0 : -1;
}

static void stream_test_run_while(coro_context_t *ctx, int (*pending)(void *), void *arg,
                                  uint64_t timeout_ms) {
    uint64_t deadline;

    if (!ctx || !pending) {
        return;
    }

    deadline = turbo_monotonic_ms() + timeout_ms;
    while (pending(arg) && turbo_monotonic_ms() < deadline) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }
}

static void stream_test_destroy_context_robust(coro_context_t *ctx) {
    int max_drain = 500;

    if (!ctx) {
        return;
    }

    coro_context_stop(ctx);
    while (max_drain-- > 0) {
        int has_handles = coro_context_alive(ctx);
        int has_coros = ctx->scheduler != NULL ? (coro_scheduler_count(ctx->scheduler) > 0) : 0;
        if (!has_handles && !has_coros) {
            break;
        }
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
    }
    coro_context_destroy(ctx);
}

static int stream_test_flag_is_pending(void *arg) {
    int *flag = (int *)arg;
    return flag && *flag == -1;
}

#ifdef _WIN32
typedef struct stream_queued_close_state_s {
    coro_socket_t *server;
    int done;
    int accept_rc;
    int recv_rc;
    int connected_after_accept;
    size_t recv_len;
    int recv_data_was_null;
} stream_queued_close_state_t;

static int stream_queued_close_pending(void *arg) {
    stream_queued_close_state_t *state = (stream_queued_close_state_t *)arg;
    return state && !state->done;
}

static int stream_raw_connect_and_close(unsigned short port) {
    SOCKET fd;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (connect(fd, (struct sockaddr *)&addr, (int)sizeof(addr)) != 0) {
        closesocket(fd);
        return -2;
    }

    shutdown(fd, SD_BOTH);
    closesocket(fd);
    return 0;
}

static void stream_accept_queued_closed_task(coro_t *co, void *arg) {
    stream_queued_close_state_t *state = (stream_queued_close_state_t *)arg;
    coro_socket_t *client = NULL;
    char *data = NULL;
    size_t len = 0U;
    (void)co;

    if (!state || !state->server) {
        return;
    }

    state->accept_rc = coro_socket_accept(state->server, &client);
    if (state->accept_rc == 0 && client != NULL) {
        state->connected_after_accept = client->connected;
        state->recv_rc = coro_socket_recv(client, &data, &len);
        state->recv_len = len;
        state->recv_data_was_null = data == NULL ? 1 : 0;
        if (data != NULL) {
            coro_socket_free_recv(data);
        }
        coro_socket_destroy(client);
    }

    state->done = 1;
}
#endif

typedef struct stream_coro_close_state_s {
    coro_context_t *ctx;
    unsigned short port;
    int handler_rc;
    int handler_hits;
    int timeout_count;
    int client_rc;
    size_t recv_len;
    int recv_data_was_null;
} stream_coro_close_state_t;

static void stream_coro_close_state_reset(stream_coro_close_state_t *state) {
    if (!state) {
        return;
    }

    state->ctx = NULL;
    state->port = 0;
    state->handler_rc = TURBO_EBUSY;
    state->handler_hits = 0;
    state->timeout_count = 0;
    state->client_rc = TURBO_EBUSY;
    state->recv_len = 0U;
    state->recv_data_was_null = 1;
}

static int stream_coro_close_waiting_pending(void *arg) {
    stream_coro_close_state_t *state = (stream_coro_close_state_t *)arg;

    if (!state) {
        return 0;
    }

    return state->handler_hits == 0;
}

static int stream_coro_close_done_pending(void *arg) {
    stream_coro_close_state_t *state = (stream_coro_close_state_t *)arg;

    if (!state) {
        return 0;
    }

    return state->handler_rc == TURBO_EBUSY;
}

static void stream_coro_recv_timeout_then_close_handler(coro_socket_t *client, void *arg) {
    stream_coro_close_state_t *state = (stream_coro_close_state_t *)arg;
    char *data = NULL;
    size_t len = 0U;
    int rc;

    if (!client || !state) {
        return;
    }

    state->handler_hits++;
    coro_socket_set_timeout(client, 5000);

    rc = coro_socket_recv(client, &data, &len);
    state->handler_rc = rc;
    state->recv_len = len;
    state->recv_data_was_null = data == NULL ? 1 : 0;
    if (data != NULL) {
        coro_socket_free_recv(data);
    }
}

#if defined(__linux__) && defined(TURBO_HAS_IO_URING)
static void stream_coro_recv_timeout_loop_handler(coro_socket_t *client, void *arg) {
    stream_coro_close_state_t *state = (stream_coro_close_state_t *)arg;
    char *data = NULL;
    size_t len = 0U;
    int rc;

    if (!client || !state) {
        return;
    }

    state->handler_hits++;
    coro_socket_set_timeout(client, 25);

    for (;;) {
        rc = coro_socket_recv(client, &data, &len);
        if (rc == TURBO_ETIMEDOUT) {
            state->timeout_count++;
            if (data != NULL) {
                coro_socket_free_recv(data);
                data = NULL;
            }
            len = 0U;
            /* Stale timeout status used to spin here forever; cap it as a test failure. */
            if (state->timeout_count > 8) {
                state->handler_rc = rc;
                state->recv_len = len;
                state->recv_data_was_null = 1;
                return;
            }
            continue;
        }

        state->handler_rc = rc;
        state->recv_len = len;
        state->recv_data_was_null = data == NULL ? 1 : 0;
        if (data != NULL) {
            coro_socket_free_recv(data);
        }
        return;
    }
}

static void stream_coro_raw_client_close_task(coro_t *co, void *arg) {
    stream_coro_close_state_t *state = (stream_coro_close_state_t *)arg;
    struct sockaddr_in addr;
    uint64_t deadline;
    int fd;
    (void)co;

    if (state == NULL || state->ctx == NULL || state->port == 0) {
        return;
    }

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        state->client_rc = -1;
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(state->port);
    if (connect(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) {
        close(fd);
        state->client_rc = -2;
        return;
    }

    state->client_rc = 0;
    deadline = turbo_monotonic_ms() + 1000;
    while (state->timeout_count == 0 && turbo_monotonic_ms() < deadline) {
        coro_sleep(state->ctx, 10);
    }
    close(fd);
}

enum {
    STREAM_IO_URING_EOF_SCENARIO_OK = 0,
    STREAM_IO_URING_EOF_SCENARIO_CONTEXT = 2,
    STREAM_IO_URING_EOF_SCENARIO_BACKEND = 3,
    STREAM_IO_URING_EOF_SCENARIO_SERVER = 4,
    STREAM_IO_URING_EOF_SCENARIO_PORT = 5,
    STREAM_IO_URING_EOF_SCENARIO_LISTEN = 6,
    STREAM_IO_URING_EOF_SCENARIO_CLIENT_SOCKET = 7,
    STREAM_IO_URING_EOF_SCENARIO_CONNECT = 8,
    STREAM_IO_URING_EOF_SCENARIO_WAITING = 9,
    STREAM_IO_URING_EOF_SCENARIO_EOF = 10,
    STREAM_IO_URING_EOF_SCENARIO_NO_HANDLER = 11,
    STREAM_IO_URING_EOF_SCENARIO_NO_TIMEOUT = 12,
    STREAM_IO_URING_EOF_SCENARIO_EARLY_RECV_RESULT = 13
};

static int stream_run_io_uring_recv_eof_scenario(void) {
    coro_context_t *ctx = NULL;
    coro_socket_t *server = NULL;
    stream_coro_close_state_t state;
    unsigned short port;
    int client_fd = -1;
    int rc = STREAM_IO_URING_EOF_SCENARIO_CONTEXT;
    struct sockaddr_in addr;

    ctx = coro_context_create(NULL);
    if (ctx == NULL) {
        goto cleanup;
    }
    rc = STREAM_IO_URING_EOF_SCENARIO_BACKEND;
    if (coro_context_set_tcp_backend(ctx, TURBO_TCP_BACKEND_IO_URING) != 0) {
        goto cleanup;
    }

    rc = STREAM_IO_URING_EOF_SCENARIO_SERVER;
    server = coro_socket_create_tcpv4(ctx);
    if (server == NULL || coro_socket_get_tcp_backend(server) != TURBO_TCP_BACKEND_IO_URING) {
        goto cleanup;
    }

    stream_coro_close_state_reset(&state);
    rc = STREAM_IO_URING_EOF_SCENARIO_PORT;
    port = stream_test_pick_loopback_port();
    if (port == 0) {
        goto cleanup;
    }
    rc = STREAM_IO_URING_EOF_SCENARIO_LISTEN;
    if (coro_socket_listen_on(server, "127.0.0.1", port,
                              stream_coro_recv_timeout_then_close_handler, &state) != 0) {
        goto cleanup;
    }

    rc = STREAM_IO_URING_EOF_SCENARIO_CLIENT_SOCKET;
    client_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client_fd < 0) {
        goto cleanup;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    rc = STREAM_IO_URING_EOF_SCENARIO_CONNECT;
    if (connect(client_fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) {
        goto cleanup;
    }

    rc = STREAM_IO_URING_EOF_SCENARIO_WAITING;
    stream_test_run_while(ctx, stream_coro_close_waiting_pending, &state, 3000);
    if (state.handler_hits != 1) {
        rc = STREAM_IO_URING_EOF_SCENARIO_NO_HANDLER;
        goto cleanup;
    }
    if (state.handler_rc != TURBO_EBUSY) {
        rc = STREAM_IO_URING_EOF_SCENARIO_EARLY_RECV_RESULT;
        goto cleanup;
    }
    close(client_fd);
    client_fd = -1;

    stream_test_run_while(ctx, stream_coro_close_done_pending, &state, 3000);
    rc = STREAM_IO_URING_EOF_SCENARIO_EOF;
    if (state.handler_rc == TURBO_EOF && state.recv_len == 0U && state.recv_data_was_null == 1) {
        rc = STREAM_IO_URING_EOF_SCENARIO_OK;
    }

cleanup:
    if (client_fd >= 0) {
        close(client_fd);
    }
    if (server != NULL) {
        coro_socket_destroy(server);
    }
    if (ctx != NULL) {
        stream_test_destroy_context_robust(ctx);
    }
    return rc;
}

static int stream_run_io_uring_timeout_loop_recv_eof_scenario(void) {
    coro_context_t *ctx = NULL;
    coro_socket_t *server = NULL;
    stream_coro_close_state_t state;
    uint64_t deadline;
    int rc = STREAM_IO_URING_EOF_SCENARIO_CONTEXT;

    ctx = coro_context_create(NULL);
    if (ctx == NULL) {
        goto cleanup;
    }
    if (coro_context_set_tcp_backend(ctx, TURBO_TCP_BACKEND_IO_URING) != 0) {
        rc = STREAM_IO_URING_EOF_SCENARIO_BACKEND;
        goto cleanup;
    }

    server = coro_socket_create_tcpv4(ctx);
    if (server == NULL || coro_socket_get_tcp_backend(server) != TURBO_TCP_BACKEND_IO_URING) {
        rc = STREAM_IO_URING_EOF_SCENARIO_SERVER;
        goto cleanup;
    }

    stream_coro_close_state_reset(&state);
    state.ctx = ctx;
    state.port = stream_test_pick_loopback_port();
    if (state.port == 0) {
        rc = STREAM_IO_URING_EOF_SCENARIO_PORT;
        goto cleanup;
    }
    if (coro_socket_listen_on(server, "127.0.0.1", state.port,
                              stream_coro_recv_timeout_loop_handler, &state) != 0) {
        rc = STREAM_IO_URING_EOF_SCENARIO_LISTEN;
        goto cleanup;
    }
    if (coro_context_spawn(ctx, stream_coro_raw_client_close_task, &state) != 0) {
        rc = STREAM_IO_URING_EOF_SCENARIO_CLIENT_SOCKET;
        goto cleanup;
    }

    deadline = turbo_monotonic_ms() + 6000;
    while (state.handler_rc == TURBO_EBUSY && turbo_monotonic_ms() < deadline) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    if (state.client_rc != 0) {
        rc = STREAM_IO_URING_EOF_SCENARIO_CONNECT;
        goto cleanup;
    }
    if (state.handler_hits != 1) {
        rc = STREAM_IO_URING_EOF_SCENARIO_NO_HANDLER;
        goto cleanup;
    }
    if (state.timeout_count == 0) {
        rc = STREAM_IO_URING_EOF_SCENARIO_NO_TIMEOUT;
        goto cleanup;
    }
    rc = STREAM_IO_URING_EOF_SCENARIO_EOF;
    if (state.handler_rc == TURBO_EOF && state.recv_len == 0U && state.recv_data_was_null == 1) {
        rc = STREAM_IO_URING_EOF_SCENARIO_OK;
    }

cleanup:
    if (server != NULL) {
        coro_socket_destroy(server);
    }
    if (ctx != NULL) {
        stream_test_destroy_context_robust(ctx);
    }
    return rc;
}
#endif

typedef struct stream_test_counts_s {
    int *connected;
    int expected_connected;
    int *accepted;
    int expected_accepted;
} stream_test_counts_t;

static int stream_test_counts_pending(void *arg) {
    stream_test_counts_t *counts = (stream_test_counts_t *)arg;

    if (!counts) {
        return 0;
    }

    return (counts->connected && *counts->connected < counts->expected_connected) ||
           (counts->accepted && *counts->accepted < counts->expected_accepted);
}

static void on_accept_local(void *server, void *client, void *peer) {
    (void)server; (void)peer;
    s_accepted_client = (turbo_stream_t *)client;
    if (s_accepted_count < (int)(sizeof(s_accepted_clients) / sizeof(s_accepted_clients[0]))) {
        s_accepted_clients[s_accepted_count] = (turbo_stream_t *)client;
    }
    s_accepted_count++;
}

static unsigned short stream_test_pick_loopback_port(void) {
    unsigned short port = 0;
    struct sockaddr_in addr;
#ifdef _WIN32
    int addr_len = (int)sizeof(addr);
    SOCKET sock = INVALID_SOCKET;
#else
    socklen_t addr_len = (socklen_t)sizeof(addr);
    int sock = -1;
#endif

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    if (sock == INVALID_SOCKET) {
        return 0;
    }
#else
    if (sock < 0) {
        return 0;
    }
#endif

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        getsockname(sock, (struct sockaddr *)&addr, &addr_len) == 0) {
        port = ntohs(addr.sin_port);
    }

#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
    return port;
}

#if defined(__linux__) || defined(__ANDROID__)
static int stream_test_raw_connect_send(unsigned short port, const char *payload) {
    struct sockaddr_in addr;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (connect(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) {
        close(fd);
        return -2;
    }

    if (send(fd, payload, strlen(payload), 0) < 0) {
        close(fd);
        return -3;
    }

    return fd;
}
#endif

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

        stream_test_run_while(ctx, stream_test_flag_is_pending, &s_connected, 3000);

        check(s_connected != 0); // Should never report a successful connect
        check_int_eq(stream_test_run_until(ctx, &s_closed, 1, 3000), 0);
        check_int_eq(s_closed, 1);
        stream_test_destroy_context_robust(ctx);
    }

#ifdef _WIN32
    it("should close tcp streams with pending connects without use-after-free") {
        enum { STREAM_CONNECT_CLOSE_LOOPS = 16 };
        int i;

        for (i = 0; i < STREAM_CONNECT_CLOSE_LOOPS; ++i) {
            coro_context_t *ctx = coro_context_create(NULL);
            turbo_stream_t *stream;
            unsigned short port;

            check_not_null(ctx);

            stream = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
            check_not_null(stream);

            port = stream_test_pick_loopback_port();
            check_int_gt(port, 0);

            s_connected = -1;
            s_closed = 0;

            check_int_eq(turbo_stream_connect(stream, "127.0.0.1", port, on_connect, on_close), 0);

            turbo_stream_close(stream);
            turbo_stream_destroy(stream);
            stream_test_destroy_context_robust(ctx);
        }
    }

    it("should close tcp streams with pending recv without use-after-free") {
        enum { STREAM_RECV_CLOSE_LOOPS = 16 };
        int i;

        for (i = 0; i < STREAM_RECV_CLOSE_LOOPS; ++i) {
            coro_context_t *ctx = coro_context_create(NULL);
            turbo_stream_listener_t *listener;
            turbo_stream_t *client;
            struct sockaddr_in addr;
            stream_test_counts_t counts;
            unsigned short port;

            check_not_null(ctx);

            port = stream_test_pick_loopback_port();
            check_int_gt(port, 0);

            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);

            s_accepted_client = NULL;
            s_accepted_count = 0;
            s_connected = -1;
            s_closed = 0;

            listener = turbo_stream_listen(ctx, TURBO_STREAM_TCP4,
                                           (struct sockaddr *)&addr, 128,
                                           on_accept_local);
            check_not_null(listener);

            client = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
            check_not_null(client);
            check_int_eq(turbo_stream_connect_addr(client, (struct sockaddr *)&addr,
                                                   on_connect, on_close), 0);

            counts.connected = &s_connected;
            counts.expected_connected = 0;
            counts.accepted = &s_accepted_count;
            counts.expected_accepted = 1;
            stream_test_run_while(ctx, stream_test_counts_pending, &counts, 3000);

            check_int_eq(s_connected, 0);
            check_int_eq(s_accepted_count, 1);
            check_not_null(s_accepted_client);

            check_int_eq(turbo_stream_recv_start(client, on_recv_noop), 0);

            turbo_stream_close(client);
            turbo_stream_destroy(client);
            turbo_stream_destroy(s_accepted_client);
            turbo_stream_listener_close(listener);
            stream_test_destroy_context_robust(ctx);
        }
    }

    it("should close tcp streams with pending send without use-after-free") {
        enum { STREAM_SEND_CLOSE_LOOPS = 8, STREAM_SEND_BURST = 8 };
        int i;

        memset(s_send_payload, 's', sizeof(s_send_payload));

        for (i = 0; i < STREAM_SEND_CLOSE_LOOPS; ++i) {
            coro_context_t *ctx = coro_context_create(NULL);
            turbo_stream_listener_t *listener;
            turbo_stream_t *client;
            struct sockaddr_in addr;
            stream_test_counts_t counts;
            unsigned short port;
            int j;

            check_not_null(ctx);

            port = stream_test_pick_loopback_port();
            check_int_gt(port, 0);

            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);

            s_accepted_client = NULL;
            s_accepted_count = 0;
            s_connected = -1;
            s_closed = 0;

            listener = turbo_stream_listen(ctx, TURBO_STREAM_TCP4,
                                           (struct sockaddr *)&addr, 128,
                                           on_accept_local);
            check_not_null(listener);

            client = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
            check_not_null(client);
            check_int_eq(turbo_stream_connect_addr(client, (struct sockaddr *)&addr,
                                                   on_connect, on_close), 0);

            counts.connected = &s_connected;
            counts.expected_connected = 0;
            counts.accepted = &s_accepted_count;
            counts.expected_accepted = 1;
            stream_test_run_while(ctx, stream_test_counts_pending, &counts, 3000);

            check_int_eq(s_connected, 0);
            check_int_eq(s_accepted_count, 1);
            check_not_null(s_accepted_client);

            for (j = 0; j < STREAM_SEND_BURST; ++j) {
                check_int_eq(turbo_stream_send(client, s_send_payload, sizeof(s_send_payload)), 0);
            }

            turbo_stream_close(client);
            turbo_stream_destroy(client);
            turbo_stream_destroy(s_accepted_client);
            turbo_stream_listener_close(listener);
            stream_test_destroy_context_robust(ctx);
        }
    }
#endif

#if defined(__linux__) || defined(__ANDROID__)
    it("should reject unavailable io_uring tcp backend") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);
#if defined(TURBO_HAS_IO_URING)
        check_int_eq(coro_context_set_tcp_backend(ctx, TURBO_TCP_BACKEND_IO_URING), 0);
#else
        check_int_eq(coro_context_set_tcp_backend(ctx, TURBO_TCP_BACKEND_IO_URING),
                     TURBO_ENOTSUP);
#endif

        coro_context_destroy(ctx);
    }
#endif

#if defined(__linux__) && defined(TURBO_HAS_IO_URING)
    it("should default tcp sockets to epoll on linux") {
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *sock;

        check(ctx != NULL);
        sock = coro_socket_create_tcpv4(ctx);
        check(sock != NULL);
        check_int_eq(coro_socket_get_tcp_backend(sock), TURBO_TCP_BACKEND_EPOLL);

        coro_socket_destroy(sock);
        coro_context_destroy(ctx);
    }
#endif

#if defined(__linux__) || defined(__ANDROID__)
    it("should preserve epoll data received before recv_start") {
        static const char payload[] = "epoll-pre-recv";
        coro_context_t *ctx = coro_context_create(NULL);
        turbo_stream_listener_t *listener;
        struct sockaddr_in addr;
        stream_test_counts_t counts;
        unsigned short port;
        int fd;
        int i;

        check_not_null(ctx);
        check_int_eq(coro_context_set_tcp_backend(ctx, TURBO_TCP_BACKEND_EPOLL), 0);

        port = stream_test_pick_loopback_port();
        check_int_gt(port, 0);

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);

        s_accepted_client = NULL;
        s_accepted_count = 0;
        s_recv_hit = 0;
        s_recv_len = 0;
        s_recv_data[0] = '\0';

        listener = turbo_stream_listen(ctx, TURBO_STREAM_TCP4,
                                       (struct sockaddr *)&addr, 128,
                                       on_accept_local);
        check_not_null(listener);

        fd = stream_test_raw_connect_send(port, payload);
        check_int_gt(fd, -1);

        counts.connected = NULL;
        counts.expected_connected = 0;
        counts.accepted = &s_accepted_count;
        counts.expected_accepted = 1;
        stream_test_run_while(ctx, stream_test_counts_pending, &counts, 3000);
        check_int_eq(s_accepted_count, 1);
        check_not_null(s_accepted_client);

        for (i = 0; i < 64; ++i) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
            usleep(1000);
        }

        check_int_eq(turbo_stream_recv_start(s_accepted_client, on_recv_capture), 0);
        stream_test_run_until(ctx, &s_recv_hit, 1, 3000);

        check_int_eq(s_recv_hit, 1);
        check_int_eq((int)s_recv_len, (int)strlen(payload));
        check_str_eq(s_recv_data, payload);

        close(fd);
        turbo_stream_destroy(s_accepted_client);
        turbo_stream_listener_close(listener);
        stream_test_destroy_context_robust(ctx);
    }
#endif

#if defined(__linux__) && defined(TURBO_HAS_IO_URING)
    it("should wake a recv waiter with eof after peer close on io_uring") {
        check_int_eq(stream_run_io_uring_recv_eof_scenario(), 0);
    }

    it("should wake a timeout-looping recv waiter with eof after peer close on io_uring") {
        check_int_eq(stream_run_io_uring_timeout_loop_recv_eof_scenario(), 0);
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
        stream_test_counts_t counts = { &s_connected, 0, &s_accepted_count, 1 };
        stream_test_run_while(ctx, stream_test_counts_pending, &counts, 3000);

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

    it("should honor reuse_port for tcp listeners") {
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *server1 = NULL;
        coro_socket_t *server2 = NULL;
        struct sockaddr_in addr;
        unsigned short port;
        int r;

        check_not_null(ctx);

        server1 = coro_socket_create_tcpv4(ctx);
        server2 = coro_socket_create_tcpv4(ctx);
        check_not_null(server1);
        check_not_null(server2);

        port = stream_test_pick_loopback_port();
        check_int_gt(port, 0);

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);

        coro_socket_set_reuse_port(server1, 1);
        coro_socket_set_reuse_port(server2, 1);

        check_int_eq(coro_socket_bind(server1, (struct sockaddr *)&addr), 0);
        r = coro_socket_listen(server1, 16);
        if (r == 0) {
            check_int_eq(coro_socket_bind(server2, (struct sockaddr *)&addr), 0);
            check_int_eq(coro_socket_listen(server2, 16), 0);
        } else {
            check(r != 0);
        }

        coro_socket_destroy(server2);
        coro_socket_destroy(server1);
        coro_context_run(ctx, TURBO_RUN_DEFAULT);
        coro_context_destroy(ctx);
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

        stream_test_counts_t counts = { &s_connected, 0, &s_accepted_count, 1 };
        stream_test_run_while(ctx, stream_test_counts_pending, &counts, 3000);

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

        stream_test_counts_t counts = { &s_connect_count, CLIENT_COUNT,
                                        &s_accepted_count, CLIENT_COUNT };
        stream_test_run_while(ctx, stream_test_counts_pending, &counts, 3000);

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

#ifdef _WIN32
    it("should close tcp listeners with pending accepts without use-after-free") {
        enum { LISTENER_CLOSE_LOOPS = 16 };
        int i;

        for (i = 0; i < LISTENER_CLOSE_LOOPS; ++i) {
            coro_context_t *ctx = coro_context_create(NULL);
            turbo_stream_listener_t *listener;
            struct sockaddr_in addr;

            check_not_null(ctx);

            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(0);

            listener = turbo_stream_listen(ctx, TURBO_STREAM_TCP4,
                                           (struct sockaddr *)&addr, 128,
                                           on_accept_local);
            check_not_null(listener);

            turbo_stream_listener_close(listener);
            stream_test_destroy_context_robust(ctx);
        }
    }

    it("should observe eof for queued accepted sockets closed before accept") {
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *server = NULL;
        struct sockaddr_in addr;
        stream_queued_close_state_t state;
        unsigned short port;
        uint64_t drain_deadline;

        check_not_null(ctx);

        port = stream_test_pick_loopback_port();
        check_int_gt(port, 0);

        server = coro_socket_create_tcpv4(ctx);
        check_not_null(server);

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);

        check_int_eq(coro_socket_bind(server, (struct sockaddr *)&addr), 0);
        check_int_eq(coro_socket_listen(server, 16), 0);
        check_int_eq(stream_raw_connect_and_close(port), 0);

        check_int_eq(stream_test_run_until(ctx, &server->accept_pending, 1, 3000), 0);

        drain_deadline = turbo_monotonic_ms() + 200;
        while (turbo_monotonic_ms() < drain_deadline) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        memset(&state, 0, sizeof(state));
        state.server = server;
        state.accept_rc = TURBO_EBUSY;
        state.recv_rc = TURBO_EBUSY;
        state.connected_after_accept = -1;
        state.recv_data_was_null = 1;

        check_int_eq(coro_context_spawn(ctx, stream_accept_queued_closed_task, &state), 0);
        stream_test_run_while(ctx, stream_queued_close_pending, &state, 3000);

        check_int_eq(state.done, 1);
        check_int_eq(state.accept_rc, 0);
        check_int_eq(state.connected_after_accept, 0);
        check_int_eq(state.recv_rc, TURBO_EOF);
        check_int_eq((int)state.recv_len, 0);
        check_int_eq(state.recv_data_was_null, 1);

        coro_socket_destroy(server);
        stream_test_destroy_context_robust(ctx);
    }
#endif
}
