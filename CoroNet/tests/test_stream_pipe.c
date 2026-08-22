#include "CoroNet.h"
#include "CoroNet/turbo_coro_internal.h"
#include "turbo_stream.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static int s_connected = -1;
static int s_closed = 0;
static turbo_stream_t *s_accepted_client = NULL;
static int s_accepted_count = 0;

static void on_connect(void *s, int status, void *arg) {
    (void)arg;
    (void)s;
    s_connected = status;
}

static void on_close(void *s) {
    (void)s;
    s_closed = 1;
}

static void on_accept_local(void *server, void *client, void *peer) {
    (void)server; (void)peer;
    s_accepted_client = (turbo_stream_t *)client;
    s_accepted_count++;
}

static int s_recv_count = 0;
static char s_recv_buf[1024];
static char s_send_payload[256 * 1024];

static int on_recv(void *stream, const mem_slice_t *slice, void *peer) {
    (void)peer;
    (void)stream;
    if (slice && slice->length > 0) {
        if (s_recv_count + slice->length < sizeof(s_recv_buf)) {
            memcpy(s_recv_buf + s_recv_count, slice->data, slice->length);
            s_recv_count += (int)slice->length;
        }
    }
    return 0;
}

static void reset_pipe_test_state(void) {
    s_accepted_client = NULL;
    s_accepted_count = 0;
    s_connected = -1;
    s_closed = 0;
    s_recv_count = 0;
    memset(s_recv_buf, 0, sizeof(s_recv_buf));
}

static unsigned long long pipe_test_unique_id(void) {
#ifdef _WIN32
    return ((unsigned long long)GetCurrentProcessId() << 32) |
           (unsigned long long)turbo_monotonic_ms();
#else
    return ((unsigned long long)getpid() << 32) | (unsigned long long)turbo_monotonic_ms();
#endif
}

static int pipe_test_close_name(char *buf, size_t size, const char *tag,
                                int index) {
#ifdef _WIN32
    return snprintf(buf, size, "\\\\.\\pipe\\turbo_test_pipe_%s_%llu_%d", tag,
                    pipe_test_unique_id(), index);
#else
    return snprintf(buf, size, "/tmp/turbo_test_pipe_%s_%llu_%d.sock", tag,
                    pipe_test_unique_id(), index);
#endif
}

static int pipe_test_run_until(coro_context_t *ctx, int *predicate, int expected,
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

static void pipe_test_run_until_idle(coro_context_t *ctx, uint64_t timeout_ms) {
    uint64_t deadline;

    if (!ctx) {
        return;
    }

    deadline = turbo_monotonic_ms() + timeout_ms;
    while (coro_context_alive(ctx) && turbo_monotonic_ms() < deadline) {
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
    }
}

static void pipe_test_destroy_context_robust(coro_context_t *ctx) {
    int max_drain = 500;

    if (!ctx) {
        return;
    }

    coro_context_stop(ctx);
    while (max_drain-- > 0) {
        if (!coro_context_alive(ctx)) {
            break;
        }
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
    }
    coro_context_destroy(ctx);
}

static void run_pipe_case(coro_context_t *ctx, const char *endpoint) {
    int r;
    const char* test_msg = "Hello Pipe!";
    size_t msg_len = strlen(test_msg);

    reset_pipe_test_state();

    turbo_stream_listener_t *listener = turbo_stream_listen_pipe(ctx, endpoint, 128, on_accept_local);
    check(listener != NULL);

#ifdef _WIN32
    Sleep(100);
#else
    usleep(100000);
#endif

    turbo_stream_t *client = turbo_stream_create(ctx, TURBO_STREAM_PIPE);
    check(client != NULL);

    r = turbo_stream_connect_pipe(client, endpoint, on_connect, on_close);
    check_equal(r, 0);

    check_equal(pipe_test_run_until(ctx, &s_connected, 0, 3000), 0);
    check_equal(pipe_test_run_until(ctx, &s_accepted_count, 1, 3000), 0);
    check_equal(s_connected, 0);
    check_equal(s_accepted_count, 1);
    check(s_accepted_client != NULL);

    r = turbo_stream_recv_start(s_accepted_client, on_recv);
    check_equal(r, 0);

    r = turbo_stream_send(client, test_msg, msg_len);
    check_equal(r, 0);

    check_equal(pipe_test_run_until(ctx, &s_recv_count, (int)msg_len, 3000), 0);
    check_equal(s_recv_count, (int)msg_len);
    check_equal(strncmp(s_recv_buf, test_msg, msg_len), 0);

    turbo_stream_destroy(client);
    if (s_accepted_client) turbo_stream_destroy(s_accepted_client);
    turbo_stream_listener_close(listener);

    check_equal(pipe_test_run_until(ctx, &s_closed, 1, 3000), 0);
    pipe_test_run_until_idle(ctx, 1000);
    check(s_closed > 0);
}

spec("Stream Pipe") {
    it("should listen and accept connections via raw pipe path") {
        coro_context_t *ctx = coro_context_create(NULL);
        char pipe_name[256];
        check(ctx != NULL);

#ifdef _WIN32
        check_true(snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\turbo_test_pipe_raw_%llu",
                            pipe_test_unique_id()) > 0);
#else
        check_true(snprintf(pipe_name, sizeof(pipe_name), "/tmp/turbo_test_pipe_raw_%llu.sock",
                            pipe_test_unique_id()) > 0);
#endif

        run_pipe_case(ctx, pipe_name);
        coro_context_destroy(ctx);
    }

    it("should accept unified pipe URL across platforms") {
        coro_context_t *ctx = coro_context_create(NULL);
        char pipe_name[256];
        check(ctx != NULL);

        check_true(snprintf(pipe_name, sizeof(pipe_name), "pipe://turbo_test_pipe_url_%llu",
                            pipe_test_unique_id()) > 0);

        run_pipe_case(ctx, pipe_name);
        coro_context_destroy(ctx);
    }

    it("should close pipe listeners with pending accepts without use-after-free") {
        enum { PIPE_LISTENER_CLOSE_LOOPS = 16 };
        int i;

        for (i = 0; i < PIPE_LISTENER_CLOSE_LOOPS; ++i) {
            coro_context_t *ctx = coro_context_create(NULL);
            turbo_stream_listener_t *listener;
            char pipe_name[256];

            check_not_null(ctx);
            check_true(pipe_test_close_name(pipe_name, sizeof(pipe_name), "close", i) > 0);

            listener = turbo_stream_listen_pipe(ctx, pipe_name, 128, on_accept_local);
            check_not_null(listener);

            turbo_stream_listener_close(listener);
            pipe_test_destroy_context_robust(ctx);
        }
    }

    it("should close pipe clients with pending connects without use-after-free") {
        enum { PIPE_CONNECT_CLOSE_LOOPS = 16 };
        int i;

        for (i = 0; i < PIPE_CONNECT_CLOSE_LOOPS; ++i) {
            coro_context_t *ctx = coro_context_create(NULL);
            turbo_stream_t *client;
            char pipe_name[256];

            check_not_null(ctx);
            client = turbo_stream_create(ctx, TURBO_STREAM_PIPE);
            check_not_null(client);

            check_true(pipe_test_close_name(pipe_name, sizeof(pipe_name), "missing", i) > 0);

            s_connected = -1;
            s_closed = 0;

            check_equal(turbo_stream_connect_pipe(client, pipe_name, on_connect, on_close), 0);

            turbo_stream_close(client);
            turbo_stream_destroy(client);
            pipe_test_destroy_context_robust(ctx);
        }
    }

    it("should close pipe clients with pending recv without use-after-free") {
        enum { PIPE_RECV_CLOSE_LOOPS = 16 };
        int i;

        for (i = 0; i < PIPE_RECV_CLOSE_LOOPS; ++i) {
            coro_context_t *ctx = coro_context_create(NULL);
            turbo_stream_listener_t *listener;
            turbo_stream_t *client;
            char pipe_name[256];

            check_not_null(ctx);
            reset_pipe_test_state();

            check_true(pipe_test_close_name(pipe_name, sizeof(pipe_name), "recv_close", i) > 0);

            listener = turbo_stream_listen_pipe(ctx, pipe_name, 128, on_accept_local);
            check_not_null(listener);

            client = turbo_stream_create(ctx, TURBO_STREAM_PIPE);
            check_not_null(client);
            check_equal(turbo_stream_connect_pipe(client, pipe_name, on_connect, on_close), 0);

            check_equal(pipe_test_run_until(ctx, &s_connected, 0, 3000), 0);
            check_equal(pipe_test_run_until(ctx, &s_accepted_count, 1, 3000), 0);
            check_equal(s_connected, 0);
            check_equal(s_accepted_count, 1);
            check_not_null(s_accepted_client);

            check_equal(turbo_stream_recv_start(client, on_recv), 0);

            turbo_stream_close(client);
            turbo_stream_destroy(client);
            turbo_stream_destroy(s_accepted_client);
            turbo_stream_listener_close(listener);
            pipe_test_destroy_context_robust(ctx);
        }
    }

    it("should close pipe clients with pending send without use-after-free") {
        enum { PIPE_SEND_CLOSE_LOOPS = 8, PIPE_SEND_BURST = 8 };
        int i;

        memset(s_send_payload, 'p', sizeof(s_send_payload));

        for (i = 0; i < PIPE_SEND_CLOSE_LOOPS; ++i) {
            coro_context_t *ctx = coro_context_create(NULL);
            turbo_stream_listener_t *listener;
            turbo_stream_t *client;
            char pipe_name[256];
            int j;

            check_not_null(ctx);
            reset_pipe_test_state();

            check_true(pipe_test_close_name(pipe_name, sizeof(pipe_name), "send_close", i) > 0);

            listener = turbo_stream_listen_pipe(ctx, pipe_name, 128, on_accept_local);
            check_not_null(listener);

            client = turbo_stream_create(ctx, TURBO_STREAM_PIPE);
            check_not_null(client);
            check_equal(turbo_stream_connect_pipe(client, pipe_name, on_connect, on_close), 0);

            check_equal(pipe_test_run_until(ctx, &s_connected, 0, 3000), 0);
            check_equal(pipe_test_run_until(ctx, &s_accepted_count, 1, 3000), 0);
            check_equal(s_connected, 0);
            check_equal(s_accepted_count, 1);
            check_not_null(s_accepted_client);

            for (j = 0; j < PIPE_SEND_BURST; ++j) {
                check_equal(turbo_stream_send(client, s_send_payload, sizeof(s_send_payload)), 0);
            }

            turbo_stream_close(client);
            turbo_stream_destroy(client);
            turbo_stream_destroy(s_accepted_client);
            turbo_stream_listener_close(listener);
            pipe_test_destroy_context_robust(ctx);
        }
    }

#ifndef _WIN32
    it("should preserve legacy ipc URL on unix") {
        coro_context_t *ctx = coro_context_create(NULL);
        char pipe_name[256];
        check(ctx != NULL);

        check_true(snprintf(pipe_name, sizeof(pipe_name),
                            "ipc:///tmp/turbo_test_pipe_ipc_%llu.sock",
                            pipe_test_unique_id()) > 0);

        run_pipe_case(ctx, pipe_name);
        coro_context_destroy(ctx);
    }
#endif
}
