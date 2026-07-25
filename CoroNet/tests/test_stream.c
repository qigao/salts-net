#include "CoroNet.h"
#include "CoroNet/turbo_coro_internal.h"
#include "turbo_stream.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
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
static const char g_zstd_payload[] = "zstd tcp payload";
#if defined(__linux__) || defined(__ANDROID__)
static size_t s_epoll_large_recv_len = 0;
static int s_epoll_large_recv_eof = 0;
static int s_epoll_large_recv_mismatch = 0;
#endif

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

#if defined(__linux__) || defined(__ANDROID__)
static int on_recv_epoll_large(void *handle, const mem_slice_t *slice, void *arg) {
    (void)handle;
    (void)arg;
    if (!slice || !slice->data) {
        s_epoll_large_recv_eof = 1;
        return 0;
    }
    for (size_t i = 0u; i < slice->length; ++i) {
        const unsigned char expected = (unsigned char)((s_epoll_large_recv_len + i) & 0xffu);
        if ((unsigned char)slice->data[i] != expected) {
            s_epoll_large_recv_mismatch = 1;
            break;
        }
    }
    s_epoll_large_recv_len += slice->length;
    return 0;
}
#endif

#define STREAM_TEST_WAIT_ITERS 20000

static unsigned short stream_test_pick_loopback_port(void);
#if defined(__linux__)
static int stream_test_read_thread_count(void);
#endif

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

typedef struct stream_write_interrupt_state_s {
    coro_socket_t *socket;
    int waiting;
    int done;
    int status;
} stream_write_interrupt_state_t;

typedef struct stream_recv_interrupt_state_s {
    coro_socket_t *socket;
    int done;
    int status;
} stream_recv_interrupt_state_t;

static void stream_recv_interrupt_task(coro_t *co, void *arg) {
    stream_recv_interrupt_state_t *state = (stream_recv_interrupt_state_t *)arg;
    char *data = NULL;
    size_t size = 0u;
    (void)co;

    state->status = coro_socket_recv(state->socket, &data, &size);
    if (data) {
        coro_socket_free_recv(data);
    }
    state->done = 1;
}

static void stream_write_interrupt_task(coro_t *co, void *arg) {
    stream_write_interrupt_state_t *state = (stream_write_interrupt_state_t *)arg;

    if (!state || !state->socket) {
        return;
    }

    state->socket->write_status = 0;
    state->socket->co_write_wait = co;
    if (coro_is_scheduled(co)) {
        coro_set_waiting_for_io(co, 1);
    }
    state->waiting = 1;
    coro_yield();
    state->status = state->socket->write_status;
    state->done = 1;
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

#if defined(__linux__) || defined(__ANDROID__)
typedef struct stream_delayed_recv_eof_state_s {
    coro_context_t *ctx;
    unsigned short port;
    int handler_done;
    int handler_rc;
    int client_done;
    int client_rc;
} stream_delayed_recv_eof_state_t;

static int stream_delayed_recv_eof_pending(void *arg) {
    stream_delayed_recv_eof_state_t *state = (stream_delayed_recv_eof_state_t *)arg;
    return state && !state->client_done;
}

static void stream_delayed_recv_eof_handler(coro_socket_t *client, void *arg) {
    stream_delayed_recv_eof_state_t *state = (stream_delayed_recv_eof_state_t *)arg;
    char *data = NULL;
    size_t size = 0u;
    if (!client || !state) return;
    state->handler_rc = coro_socket_recv(client, &data, &size);
    if (data) coro_socket_free_recv(data);
    state->handler_done = 1;
}

static void stream_delayed_recv_eof_client(coro_t *co, void *arg) {
    static const char request[] = "close-before-recv";
    stream_delayed_recv_eof_state_t *state = (stream_delayed_recv_eof_state_t *)arg;
    coro_socket_t *client = NULL;
    char *data = NULL;
    size_t size = 0u;
    uint64_t deadline;
    (void)co;
    if (!state || !state->ctx) return;
    client = coro_socket_create_tcpv4(state->ctx);
    if (!client) {
        state->client_rc = TURBO_ENOMEM;
        state->client_done = 1;
        return;
    }
    coro_socket_set_timeout(client, 3000u);
    state->client_rc = coro_socket_connect(client, "127.0.0.1", state->port);
    if (state->client_rc == TURBO_OK)
        state->client_rc = coro_socket_send(client, request, sizeof(request) - 1u);
    deadline = turbo_monotonic_ms() + 1000u;
    while (state->client_rc == TURBO_OK && !state->handler_done &&
           turbo_monotonic_ms() < deadline)
        coro_sleep(state->ctx, 1u);
    if (state->client_rc == TURBO_OK) {
        /* Let the peer close and let epoll deliver the terminal event before
         * coro_socket_recv installs the transport receive callback. */
        coro_sleep(state->ctx, 20u);
        state->client_rc = coro_socket_recv(client, &data, &size);
    }
    if (data) coro_socket_free_recv(data);
    coro_socket_destroy(client);
    state->client_done = 1;
}
#endif

typedef struct stream_zstd_echo_state_s {
    coro_context_t *ctx;
    unsigned short port;
    int handler_done;
    int client_done;
    int handler_rc;
    int client_rc;
    int zstd_disabled_send_rc;
    int zstd_level_set_rc;
    int client_recv_rc;
    char recv_data[128];
    size_t recv_len;
} stream_zstd_echo_state_t;

static int stream_test_zstd_echo_pending(void *arg) {
    stream_zstd_echo_state_t *state = (stream_zstd_echo_state_t *)arg;
    if (!state) return 1;
    return !(state->handler_done && state->client_done);
}

static void stream_zstd_echo_server_handler(coro_socket_t *client, void *arg) {
    stream_zstd_echo_state_t *state = (stream_zstd_echo_state_t *)arg;
    char *data = NULL;
    size_t len = 0U;
    int rc;

    if (!client) {
        return;
    }

    if (!state) {
        coro_socket_destroy(client);
        return;
    }

    rc = coro_socket_recv_compressed(client, &data, &len);
    if (rc == 0 && data != NULL) {
        state->handler_rc = coro_socket_send_compressed(client, data, len);
    } else {
        state->handler_rc = rc;
    }

    if (data != NULL) {
        coro_socket_free_recv(data);
    }

    state->handler_done = 1;
    coro_socket_destroy(client);
}

static void stream_zstd_echo_client_task(coro_t *co, void *arg) {
    stream_zstd_echo_state_t *state = (stream_zstd_echo_state_t *)arg;
    coro_socket_t *client = NULL;
    char *data = NULL;
    size_t len = 0U;
    int rc;
    (void)co;

    if (!state || !state->ctx) {
        return;
    }

    client = coro_socket_create(state->ctx, CORO_SOCKET_TCP_V4);
    if (!client) {
        state->client_rc = TURBO_ENOMEM;
        state->client_done = 1;
        return;
    }

    rc = coro_socket_connect(client, "127.0.0.1", state->port);
    if (rc != 0) {
        state->client_rc = rc;
        coro_socket_destroy(client);
        state->client_done = 1;
        return;
    }

    state->zstd_level_set_rc = coro_socket_set_compression_level(client, 0);
    if (state->zstd_level_set_rc == 0) {
        state->zstd_disabled_send_rc = coro_socket_send_compressed(client, g_zstd_payload,
                                                                  sizeof(g_zstd_payload) - 1U);
    } else {
        state->zstd_disabled_send_rc = state->zstd_level_set_rc;
    }
    if (state->zstd_level_set_rc == 0) {
        state->zstd_level_set_rc = coro_socket_set_compression_level(client, 1);
    }
    if (state->zstd_level_set_rc != 0) {
        state->client_rc = state->zstd_level_set_rc;
        state->client_done = 1;
        coro_socket_destroy(client);
        return;
    }

    rc = coro_socket_send_compressed(client, g_zstd_payload, sizeof(g_zstd_payload) - 1U);
    if (rc != 0) {
        state->client_rc = rc;
        coro_socket_destroy(client);
        state->client_done = 1;
        return;
    }

    state->client_recv_rc = coro_socket_recv_compressed(client, &data, &len);
    if (state->client_recv_rc == 0 && data != NULL && len > 0U) {
        size_t copy_len = (len < (sizeof(state->recv_data) - 1U)) ? len
                                                                : (sizeof(state->recv_data) - 1U);
        memcpy(state->recv_data, data, copy_len);
        state->recv_data[copy_len] = '\0';
        state->recv_len = copy_len;
    }
    state->client_rc = state->client_recv_rc;

    if (data != NULL) {
        coro_socket_free_recv(data);
    }
    state->client_done = 1;
    coro_socket_destroy(client);
}

typedef struct stream_zstd_auto_echo_state_s {
    coro_context_t *ctx;
    unsigned short port;
    int handler_done;
    int client_done;
    int handler_rc;
    int client_rc;
    int client_set_level_rc;
    char recv_data[128];
    size_t recv_len;
} stream_zstd_auto_echo_state_t;

static int stream_zstd_auto_echo_pending(void *arg) {
    stream_zstd_auto_echo_state_t *state = (stream_zstd_auto_echo_state_t *)arg;
    if (!state) return 1;
    return !(state->handler_done && state->client_done);
}

static void stream_zstd_auto_echo_server_handler(coro_socket_t *client, void *arg) {
    stream_zstd_auto_echo_state_t *state = (stream_zstd_auto_echo_state_t *)arg;
    char *data = NULL;
    size_t len = 0U;
    int rc;

    if (!client) {
        return;
    }

    if (!state) {
        coro_socket_destroy(client);
        return;
    }

    rc = coro_socket_set_compression_level(client, 1);
    if (rc == 0) {
        rc = coro_socket_recv(client, &data, &len);
    } else {
        state->handler_rc = rc;
    }

    if (rc == 0 && data != NULL) {
        state->handler_rc = coro_socket_send(client, data, len);
    } else if (rc == 0) {
        state->handler_rc = 0;
    } else {
        state->handler_rc = rc;
    }

    if (data != NULL) {
        coro_socket_free_recv(data);
    }

    state->handler_done = 1;
    coro_socket_destroy(client);
}

static void stream_zstd_auto_echo_client_task(coro_t *co, void *arg) {
    stream_zstd_auto_echo_state_t *state = (stream_zstd_auto_echo_state_t *)arg;
    coro_socket_t *client = NULL;
    char *data = NULL;
    size_t len = 0U;
    int rc;
    (void)co;

    if (!state || !state->ctx) {
        return;
    }

    client = coro_socket_create(state->ctx, CORO_SOCKET_TCP_V4);
    if (!client) {
        state->client_rc = TURBO_ENOMEM;
        state->client_done = 1;
        return;
    }

    rc = coro_socket_connect(client, "127.0.0.1", state->port);
    if (rc != 0) {
        state->client_rc = rc;
        coro_socket_destroy(client);
        state->client_done = 1;
        return;
    }

    state->client_set_level_rc = coro_socket_set_compression_level(client, 1);
    if (state->client_set_level_rc != 0) {
        state->client_rc = state->client_set_level_rc;
        state->client_done = 1;
        coro_socket_destroy(client);
        return;
    }

    rc = coro_socket_send(client, g_zstd_payload, sizeof(g_zstd_payload) - 1U);
    if (rc != 0) {
        state->client_rc = rc;
        coro_socket_destroy(client);
        state->client_done = 1;
        return;
    }

    rc = coro_socket_recv(client, &data, &len);
    if (rc == 0 && data != NULL && len > 0U) {
        size_t copy_len = (len < (sizeof(state->recv_data) - 1U)) ? len
                                                                : (sizeof(state->recv_data) - 1U);
        memcpy(state->recv_data, data, copy_len);
        state->recv_data[copy_len] = '\0';
        state->recv_len = copy_len;
    }
    state->client_rc = rc;

    if (data != NULL) {
        coro_socket_free_recv(data);
    }

    state->client_done = 1;
    coro_socket_destroy(client);
}

typedef struct stream_sendv_state_s {
    coro_context_t *ctx;
    unsigned short port;
    int handler_done;
    int client_done;
    int handler_rc;
    int client_rc;
    const turbo_iovec_t *slices;
    size_t slice_count;
    char *received;
    size_t received_capacity;
    size_t expected_len;
    size_t received_len;
} stream_sendv_state_t;

static int stream_sendv_pending(void *arg) {
    stream_sendv_state_t *state = (stream_sendv_state_t *)arg;
    return !state || !(state->handler_done && state->client_done);
}

static void stream_sendv_server_handler(coro_socket_t *client, void *arg) {
    stream_sendv_state_t *state = (stream_sendv_state_t *)arg;

    if (!client || !state) {
        if (client) coro_socket_destroy(client);
        return;
    }
    while (state->received_len < state->expected_len) {
        char *data = NULL;
        size_t len = 0U;
        int rc = coro_socket_recv(client, &data, &len);
        if (rc != 0) {
            state->handler_rc = rc;
            break;
        }
        if (len == 0U) {
            state->handler_rc = TURBO_EOF;
            if (data) coro_socket_free_recv(data);
            break;
        }
        if (len > state->received_capacity - state->received_len) {
            state->handler_rc = TURBO_ENOBUFS;
            coro_socket_free_recv(data);
            break;
        }
        memcpy(state->received + state->received_len, data, len);
        state->received_len += len;
        coro_socket_free_recv(data);
    }
    state->handler_done = 1;
    coro_socket_destroy(client);
}

static void stream_sendv_client_task(coro_t *co, void *arg) {
    stream_sendv_state_t *state = (stream_sendv_state_t *)arg;
    coro_socket_t *client;
    int rc;
    (void)co;

    if (!state || !state->ctx || !state->slices || state->slice_count == 0U) return;
    client = coro_socket_create_tcpv4(state->ctx);
    if (!client) {
        state->client_rc = TURBO_ENOMEM;
        state->client_done = 1;
        return;
    }
    rc = coro_socket_connect(client, "127.0.0.1", state->port);
    if (rc == 0) rc = coro_socket_sendv(client, state->slices, state->slice_count);
    state->client_rc = rc;
    state->client_done = 1;
    coro_socket_destroy(client);
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

#if defined(__linux__)
static int stream_test_read_thread_count(void) {
    FILE *fp = fopen("/proc/self/status", "r");
    char line[128];
    int threads = -1;

    if (!fp) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "Threads:%d", &threads) == 1) {
            break;
        }
    }

    fclose(fp);
    return threads;
}

enum {
    STREAM_THREAD_SCALE_CLIENTS = 24,
    STREAM_THREAD_SCALE_TIMEOUT_MS = 3000
};

static int stream_test_backend_thread_growth(turbo_tcp_backend_t backend,
                                             int *thread_growth) {
    coro_context_t *ctx = NULL;
    turbo_stream_listener_t *listener = NULL;
    turbo_stream_t *clients[STREAM_THREAD_SCALE_CLIENTS] = {0};
    int connect_status[STREAM_THREAD_SCALE_CLIENTS];
    stream_test_counts_t counts;
    struct sockaddr_in addr;
    unsigned short port;
    int threads_before;
    int threads_after;
    int created = 0;
    int rc = 1;
    int i;

    if (!thread_growth) {
        return TURBO_EINVAL;
    }
    *thread_growth = -1;

    ctx = coro_context_create(NULL);
    if (!ctx || coro_context_set_tcp_backend(ctx, backend) != TURBO_OK) {
        goto cleanup;
    }

    threads_before = stream_test_read_thread_count();
    if (threads_before <= 0) {
        rc = 2;
        goto cleanup;
    }
    port = stream_test_pick_loopback_port();
    if (port == 0) {
        rc = 3;
        goto cleanup;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    memset(s_accepted_clients, 0, sizeof(s_accepted_clients));
    memset(connect_status, 0xFF, sizeof(connect_status));
    s_accepted_client = NULL;
    s_accepted_count = 0;
    s_connect_count = 0;

    listener = turbo_stream_listen(ctx, TURBO_STREAM_TCP4,
                                   (struct sockaddr *)&addr, 128, on_accept_local);
    if (!listener) {
        rc = 4;
        goto cleanup;
    }

    for (i = 0; i < STREAM_THREAD_SCALE_CLIENTS; ++i) {
        clients[i] = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
        if (!clients[i]) {
            rc = 5;
            goto cleanup;
        }
        created++;
        turbo_stream_set_user_data(clients[i], &connect_status[i]);
        if (turbo_stream_connect_addr(clients[i], (struct sockaddr *)&addr,
                                      on_connect_count, on_close) != TURBO_OK) {
            rc = 6;
            goto cleanup;
        }
    }

    counts.connected = &s_connect_count;
    counts.expected_connected = STREAM_THREAD_SCALE_CLIENTS;
    counts.accepted = &s_accepted_count;
    counts.expected_accepted = STREAM_THREAD_SCALE_CLIENTS;
    stream_test_run_while(ctx, stream_test_counts_pending, &counts,
                          STREAM_THREAD_SCALE_TIMEOUT_MS);
    if (s_connect_count != STREAM_THREAD_SCALE_CLIENTS ||
        s_accepted_count != STREAM_THREAD_SCALE_CLIENTS) {
        rc = 7;
        goto cleanup;
    }

    threads_after = stream_test_read_thread_count();
    if (threads_after <= 0) {
        rc = 8;
        goto cleanup;
    }
    *thread_growth = threads_after - threads_before;
    rc = TURBO_OK;

cleanup:
    for (i = 0; i < created; ++i) {
        if (clients[i]) {
            turbo_stream_destroy(clients[i]);
        }
    }
    for (i = 0; i < s_accepted_count &&
                i < (int)(sizeof(s_accepted_clients) / sizeof(s_accepted_clients[0])); ++i) {
        if (s_accepted_clients[i]) {
            turbo_stream_destroy(s_accepted_clients[i]);
        }
    }
    if (listener) {
        turbo_stream_listener_close(listener);
    }
    stream_test_destroy_context_robust(ctx);
    return rc;
}
#endif

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

static int stream_test_raw_connect(unsigned short port) {
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) {
        close(fd);
        return -2;
    }
    return fd;
}

static int stream_test_raw_send_all(int fd, const uint8_t *payload, size_t payload_size) {
    size_t offset = 0u;
    if (fd < 0 || !payload || payload_size == 0u) return -1;
    while (offset < payload_size) {
        ssize_t sent = send(fd, payload + offset, payload_size - offset, 0);
        if (sent <= 0) return -2;
        offset += (size_t)sent;
    }
    return 0;
}

static int stream_test_raw_connect_send_bytes(unsigned short port, const uint8_t *payload,
                                              size_t payload_size) {
    int fd = stream_test_raw_connect(port);
    if (fd < 0) return fd;
    if (stream_test_raw_send_all(fd, payload, payload_size) != 0) {
        close(fd);
        return -3;
    }
    if (shutdown(fd, SHUT_WR) != 0) {
        close(fd);
        return -4;
    }
    return fd;
}
#endif

spec("Stream") {
    it("delivers a recv interrupt posted before the waiter is armed") {
        stream_recv_interrupt_state_t state;
        coro_context_t *ctx = coro_context_create(NULL);

        memset(&state, 0, sizeof(state));
        check_not_null(ctx);
        state.socket = coro_socket_create_tcpv4(ctx);
        check_not_null(state.socket);

        check_int_eq(coro_socket_interrupt_wait(state.socket, TURBO_EINTR), 0);
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
        check_null(state.socket->co_wait);
        check_int_eq(coro_context_spawn(ctx, stream_recv_interrupt_task, &state), 0);
        check_int_eq(stream_test_run_until(ctx, &state.done, 1, 1000), 0);
        check_int_eq(state.status, TURBO_EINTR);

        coro_socket_destroy(state.socket);
        stream_test_destroy_context_robust(ctx);
    }

    it("should interrupt a pending coroutine write wait") {
        stream_write_interrupt_state_t state;
        coro_context_t *ctx = coro_context_create(NULL);

        memset(&state, 0, sizeof(state));
        check_not_null(ctx);
        state.socket = coro_socket_create_tcpv4(ctx);
        check_not_null(state.socket);

        check_int_eq(coro_context_spawn(ctx, stream_write_interrupt_task, &state), 0);
        check_int_eq(stream_test_run_until(ctx, &state.waiting, 1, 1000), 0);
        check_null(state.socket->co_wait);
        check_not_null(state.socket->co_write_wait);

        check_int_eq(coro_socket_interrupt_wait(state.socket, TURBO_ECANCELED), 0);
        check_int_eq(stream_test_run_until(ctx, &state.done, 1, 1000), 0);
        check_int_eq(state.status, TURBO_ECANCELED);
        check_null(state.socket->co_write_wait);

        coro_socket_destroy(state.socket);
        stream_test_destroy_context_robust(ctx);
    }

    it("queues a recv control interrupt without canceling a pending write") {
        stream_write_interrupt_state_t write_state;
        stream_recv_interrupt_state_t recv_state;
        coro_context_t *ctx = coro_context_create(NULL);

        memset(&write_state, 0, sizeof(write_state));
        memset(&recv_state, 0, sizeof(recv_state));
        check_not_null(ctx);
        write_state.socket = coro_socket_create_tcpv4(ctx);
        check_not_null(write_state.socket);
        recv_state.socket = write_state.socket;

        check_int_eq(coro_context_spawn(ctx, stream_write_interrupt_task, &write_state), 0);
        check_int_eq(stream_test_run_until(ctx, &write_state.waiting, 1, 1000), 0);
        check_int_eq(coro_socket_interrupt_wait(write_state.socket, TURBO_EINTR), 0);
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
        check_int_eq(write_state.done, 0);
        check_not_null(write_state.socket->co_write_wait);
        check_int_eq(write_state.socket->pending_recv_interrupt, 1);

        check_int_eq(coro_socket_interrupt_wait(write_state.socket, TURBO_ECANCELED), 0);
        check_int_eq(stream_test_run_until(ctx, &write_state.done, 1, 1000), 0);
        check_int_eq(write_state.status, TURBO_ECANCELED);
        check_int_eq(coro_context_spawn(ctx, stream_recv_interrupt_task, &recv_state), 0);
        check_int_eq(stream_test_run_until(ctx, &recv_state.done, 1, 1000), 0);
        check_int_eq(recv_state.status, TURBO_EINTR);

        coro_socket_destroy(write_state.socket);
        stream_test_destroy_context_robust(ctx);
    }

    it("should create and destroy stream") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        turbo_stream_t *stream = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
        check(stream != NULL);

        turbo_stream_destroy(stream);
        coro_context_destroy(ctx);
    }

    it("should allocate context-sized stream receive buffers") {
        coro_context_t *ctx = coro_context_create(NULL);
        mem_pool_t *arena;
        turbo_stream_t *stream;
        size_t before;
        size_t after;

        check_not_null(ctx);
        check_int_eq(coro_context_set_stream_recv_buffer_size(ctx, 4096u), TURBO_OK);
        arena = (mem_pool_t *)coro_context_get_arena(ctx);
        check_not_null(arena);
        before = mem_pool_total_used(arena);
        stream = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
        check_not_null(stream);
        after = mem_pool_total_used(arena);
        check_size_gt(after, before);
        check_size_lt(after - before, 64u * 1024u);

        turbo_stream_destroy(stream);
        coro_context_destroy(ctx);
    }

    it("should expose TCP keepalive linger and send HWM socket options") {
        turbo_tcp_keepalive_config_t keepalive;
        turbo_socket_linger_config_t linger;
        coro_context_t *ctx = coro_context_create(NULL);
        turbo_stream_t *stream = NULL;
        coro_socket_t *tcp = NULL;
        coro_socket_t *udp = NULL;

        memset(&keepalive, 0, sizeof(keepalive));
        memset(&linger, 0, sizeof(linger));
        check(ctx != NULL);

        stream = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
        check(stream != NULL);
        check_int_eq(turbo_stream_set_send_hwm(stream, 4), 0);
        check_int_eq(turbo_stream_send(stream, "abcde", 5), TURBO_ENOBUFS);
        keepalive.enabled = 1;
        keepalive.idle_ms = 1000;
        keepalive.interval_ms = 1000;
        keepalive.count = 3;
        check_int_eq(turbo_stream_set_tcp_keepalive(stream, &keepalive), 0);
        linger.enabled = 1;
        linger.timeout_ms = 1000;
        check_int_eq(turbo_stream_set_linger(stream, &linger), 0);
        check_int_eq(turbo_stream_set_recv_buffer_size(stream, 1024u * 1024u), 0);
        check_int_eq(turbo_stream_set_send_buffer_size(stream, 512u * 1024u), 0);
        check_int_eq(turbo_stream_set_recv_buffer_size(stream, 0u), TURBO_EINVAL);
        check_int_eq(turbo_stream_set_send_buffer_size(stream, (size_t)INT32_MAX + 1u),
                     TURBO_ERANGE);
        turbo_stream_destroy(stream);

        tcp = coro_socket_create_tcpv4(ctx);
        udp = coro_socket_create_udpv4(ctx);
        check(tcp != NULL);
        check(udp != NULL);
        check_int_eq(coro_socket_set_tcp_keepalive(tcp, &keepalive), 0);
        check_int_eq(coro_socket_set_linger(tcp, &linger), 0);
        check_int_eq(coro_socket_set_recv_buffer_size(tcp, 1024u * 1024u), 0);
        check_int_eq(coro_socket_set_send_buffer_size(tcp, 512u * 1024u), 0);
        check_int_eq(coro_socket_set_send_hwm(tcp, 4), 0);
        check_int_eq(coro_socket_set_tcp_keepalive(udp, &keepalive), TURBO_ENOTSUP);
        check_int_eq(coro_socket_set_linger(udp, &linger), TURBO_ENOTSUP);
        check_int_eq(coro_socket_set_recv_buffer_size(udp, 1024u), TURBO_ENOTSUP);
        check_int_eq(coro_socket_set_send_buffer_size(udp, 1024u), TURBO_ENOTSUP);
        check_int_eq(coro_socket_set_send_hwm(udp, 4), TURBO_ENOTSUP);
        coro_socket_destroy(tcp);
        coro_socket_destroy(udp);
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
    it("should preserve payloads larger than the epoll read ring before EOF") {
        enum { STREAM_EPOLL_LARGE_PAYLOAD_BYTES = 512u * 1024u };
        coro_context_t *ctx = coro_context_create(NULL);
        turbo_stream_listener_t *listener;
        struct sockaddr_in addr;
        stream_test_counts_t counts;
        unsigned char *payload;
        unsigned short port;
        uint64_t deadline;
        int fd;

        check_not_null(ctx);
        check_int_eq(coro_context_set_tcp_backend(ctx, TURBO_TCP_BACKEND_EPOLL), 0);
        payload = (unsigned char *)malloc(STREAM_EPOLL_LARGE_PAYLOAD_BYTES);
        check_not_null(payload);
        for (size_t i = 0u; i < STREAM_EPOLL_LARGE_PAYLOAD_BYTES; ++i)
            payload[i] = (unsigned char)(i & 0xffu);

        port = stream_test_pick_loopback_port();
        check_int_gt(port, 0);
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        s_accepted_client = NULL;
        s_accepted_count = 0;
        s_epoll_large_recv_len = 0u;
        s_epoll_large_recv_eof = 0;
        s_epoll_large_recv_mismatch = 0;
        listener = turbo_stream_listen(ctx, TURBO_STREAM_TCP4, (struct sockaddr *)&addr, 128,
                                       on_accept_local);
        check_not_null(listener);
        fd = stream_test_raw_connect_send_bytes(port, payload,
                                                STREAM_EPOLL_LARGE_PAYLOAD_BYTES);
        check_int_gt(fd, -1);
        counts.connected = NULL;
        counts.expected_connected = 0;
        counts.accepted = &s_accepted_count;
        counts.expected_accepted = 1;
        stream_test_run_while(ctx, stream_test_counts_pending, &counts, 3000);
        check_int_eq(s_accepted_count, 1);
        check_not_null(s_accepted_client);
        check_int_eq(turbo_stream_recv_start(s_accepted_client, on_recv_epoll_large), 0);
        deadline = turbo_monotonic_ms() + 5000u;
        while ((!s_epoll_large_recv_eof ||
                s_epoll_large_recv_len != STREAM_EPOLL_LARGE_PAYLOAD_BYTES) &&
               turbo_monotonic_ms() < deadline)
            coro_context_run(ctx, TURBO_RUN_ONCE);
        check_int_eq(s_epoll_large_recv_mismatch, 0);
        check_size_eq(s_epoll_large_recv_len, STREAM_EPOLL_LARGE_PAYLOAD_BYTES);
        check_int_eq(s_epoll_large_recv_eof, 1);

        close(fd);
        free(payload);
        turbo_stream_destroy(s_accepted_client);
        turbo_stream_listener_close(listener);
        stream_test_destroy_context_robust(ctx);
    }

    it("should continue epoll reads after a full ring without waiting for EOF") {
        enum {
            STREAM_EPOLL_OPEN_PAYLOAD_BYTES = 512u * 1024u,
            STREAM_EPOLL_OPEN_TAIL_BYTES = 257u,
            STREAM_EPOLL_OPEN_TOTAL_BYTES =
                STREAM_EPOLL_OPEN_PAYLOAD_BYTES + STREAM_EPOLL_OPEN_TAIL_BYTES
        };
        coro_context_t *ctx = coro_context_create(NULL);
        turbo_stream_listener_t *listener;
        struct sockaddr_in addr;
        stream_test_counts_t counts;
        unsigned char *payload;
        unsigned short port;
        uint64_t deadline;
        int fd;

        check_not_null(ctx);
        check_int_eq(coro_context_set_tcp_backend(ctx, TURBO_TCP_BACKEND_EPOLL), 0);
        payload = (unsigned char *)malloc(STREAM_EPOLL_OPEN_TOTAL_BYTES);
        check_not_null(payload);
        for (size_t i = 0u; i < STREAM_EPOLL_OPEN_TOTAL_BYTES; ++i)
            payload[i] = (unsigned char)(i & 0xffu);

        port = stream_test_pick_loopback_port();
        check_int_gt(port, 0);
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        s_accepted_client = NULL;
        s_accepted_count = 0;
        s_epoll_large_recv_len = 0u;
        s_epoll_large_recv_eof = 0;
        s_epoll_large_recv_mismatch = 0;
        listener = turbo_stream_listen(ctx, TURBO_STREAM_TCP4, (struct sockaddr *)&addr, 128,
                                       on_accept_local);
        check_not_null(listener);
        fd = stream_test_raw_connect(port);
        check_int_gt(fd, -1);
        check_int_eq(stream_test_raw_send_all(fd, payload, STREAM_EPOLL_OPEN_PAYLOAD_BYTES), 0);
        counts.connected = NULL;
        counts.expected_connected = 0;
        counts.accepted = &s_accepted_count;
        counts.expected_accepted = 1;
        stream_test_run_while(ctx, stream_test_counts_pending, &counts, 3000);
        check_int_eq(s_accepted_count, 1);
        check_not_null(s_accepted_client);
        check_int_eq(turbo_stream_recv_start(s_accepted_client, on_recv_epoll_large), 0);
        deadline = turbo_monotonic_ms() + 5000u;
        while (s_epoll_large_recv_len != STREAM_EPOLL_OPEN_PAYLOAD_BYTES &&
               turbo_monotonic_ms() < deadline)
            coro_context_run(ctx, TURBO_RUN_ONCE);
        check_int_eq(s_epoll_large_recv_mismatch, 0);
        check_size_eq(s_epoll_large_recv_len, STREAM_EPOLL_OPEN_PAYLOAD_BYTES);
        check_int_eq(s_epoll_large_recv_eof, 0);

        check_int_eq(stream_test_raw_send_all(fd, payload + STREAM_EPOLL_OPEN_PAYLOAD_BYTES,
                                              STREAM_EPOLL_OPEN_TAIL_BYTES),
                     0);
        deadline = turbo_monotonic_ms() + 3000u;
        while (s_epoll_large_recv_len != STREAM_EPOLL_OPEN_TOTAL_BYTES &&
               turbo_monotonic_ms() < deadline)
            coro_context_run(ctx, TURBO_RUN_ONCE);
        check_int_eq(s_epoll_large_recv_mismatch, 0);
        check_size_eq(s_epoll_large_recv_len, STREAM_EPOLL_OPEN_TOTAL_BYTES);
        check_int_eq(s_epoll_large_recv_eof, 0);

        check_int_eq(shutdown(fd, SHUT_WR), 0);
        deadline = turbo_monotonic_ms() + 3000u;
        while (!s_epoll_large_recv_eof && turbo_monotonic_ms() < deadline)
            coro_context_run(ctx, TURBO_RUN_ONCE);
        check_int_eq(s_epoll_large_recv_eof, 1);

        close(fd);
        free(payload);
        turbo_stream_destroy(s_accepted_client);
        turbo_stream_listener_close(listener);
        stream_test_destroy_context_robust(ctx);
    }

    it("should preserve epoll EOF delivered before recv_start") {
        stream_delayed_recv_eof_state_t state;
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *server;

        memset(&state, 0, sizeof(state));
        check_not_null(ctx);
        check_int_eq(coro_context_set_tcp_backend(ctx, TURBO_TCP_BACKEND_EPOLL), 0);
        state.ctx = ctx;
        state.port = stream_test_pick_loopback_port();
        state.handler_rc = TURBO_EBUSY;
        state.client_rc = TURBO_EBUSY;
        check_int_gt(state.port, 0);
        server = coro_socket_create_tcpv4(ctx);
        check_not_null(server);
        check_int_eq(coro_socket_listen_on(server, "127.0.0.1", state.port,
                                           stream_delayed_recv_eof_handler, &state),
                     0);
        check_int_eq(coro_context_spawn(ctx, stream_delayed_recv_eof_client, &state), 0);
        stream_test_run_while(ctx, stream_delayed_recv_eof_pending, &state, 3000u);
        check_int_eq(state.handler_done, 1);
        check_int_eq(state.handler_rc, TURBO_OK);
        check_int_eq(state.client_done, 1);
        check_int_eq(state.client_rc, TURBO_EOF);

        coro_socket_destroy(server);
        stream_test_destroy_context_robust(ctx);
    }

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
        unsigned short port;
        check(ctx != NULL);

        struct sockaddr_in addr;
        port = stream_test_pick_loopback_port();
        check_int_gt(port, 0);

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port);

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
        struct sockaddr_storage addr;
        const turbo_iovec_t iov = {"x", 1U};

        check_int_eq(coro_socket_connect_pipe(NULL, "\\\\.\\pipe\\missing"), TURBO_EINVAL);
        check_int_eq(coro_socket_connect_ws(NULL, "127.0.0.1", 80, "/", 0), TURBO_EINVAL);
        check_int_eq(coro_socket_send(NULL, "x", 1), TURBO_EINVAL);
        check_int_eq(coro_socket_sendv(NULL, &iov, 1U), TURBO_EINVAL);
        check(coro_socket_get_send_buffer(NULL, 16) == NULL);
        check_int_eq(coro_socket_send_buffer(NULL, NULL, 0), TURBO_EINVAL);
        check_int_eq(coro_socket_recv(NULL, NULL, NULL), TURBO_EINVAL);
        check_int_eq(coro_socket_bind(NULL, NULL), TURBO_EINVAL);
        check_int_eq(coro_socket_listen(NULL, 1), TURBO_EINVAL);
        check_int_eq(coro_socket_accept(NULL, NULL), TURBO_EINVAL);
        check(coro_socket_get_context(NULL) == NULL);
        coro_socket_set_user_data(NULL, NULL);
        check(coro_socket_get_user_data(NULL) == NULL);
        check_int_eq(coro_socket_get_local_address(NULL, &addr), TURBO_EINVAL);
        check_int_eq(coro_socket_get_local_address((coro_socket_t *)1, NULL), TURBO_EINVAL);
    }

    it("should concatenate TCP send vectors into one completed write") {
        coro_context_t *ctx = coro_context_create(NULL);
        stream_sendv_state_t state;
        coro_socket_t *server;
        char received[32];
        const turbo_iovec_t slices[] = {{"hello", 5U}, {NULL, 0U}, {" ", 1U}, {"world", 5U}};

        check_not_null(ctx);
        memset(&state, 0, sizeof(state));
        memset(received, 0, sizeof(received));
        state.ctx = ctx;
        state.slices = slices;
        state.slice_count = sizeof(slices) / sizeof(slices[0]);
        state.received = received;
        state.received_capacity = sizeof(received);
        state.expected_len = 11U;
        state.port = stream_test_pick_loopback_port();
        check_int_gt(state.port, 0);
        server = coro_socket_create_tcpv4(ctx);
        check_not_null(server);
        check_int_eq(coro_socket_listen_on(server, "127.0.0.1", state.port,
                                           stream_sendv_server_handler, &state), 0);
        check_int_eq(coro_context_spawn(ctx, stream_sendv_client_task, &state), 0);
        stream_test_run_while(ctx, stream_sendv_pending, &state, 3000U);
        check_int_eq(state.client_rc, 0);
        check_int_eq(state.handler_rc, 0);
        check_true(state.client_done);
        check_true(state.handler_done);
        check_size_eq(state.received_len, 11U);
        check_mem_eq(state.received, "hello world", 11U);
        coro_socket_destroy(server);
        stream_test_destroy_context_robust(ctx);
    }

#ifdef _WIN32
    it("should preserve large borrowed TCP vectors until IOCP completes") {
        enum {
            SENDV_SLICE_COUNT = 32,
            SENDV_SLICE_BYTES = 4096,
            SENDV_TOTAL_BYTES = SENDV_SLICE_COUNT * SENDV_SLICE_BYTES
        };
        coro_context_t *ctx = coro_context_create(NULL);
        stream_sendv_state_t state;
        coro_socket_t *server;
        turbo_iovec_t *slices = NULL;
        char *payload = NULL;
        char *received = NULL;

        check_not_null(ctx);
        slices = (turbo_iovec_t *)calloc(SENDV_SLICE_COUNT, sizeof(*slices));
        payload = (char *)malloc(SENDV_TOTAL_BYTES);
        received = (char *)calloc(SENDV_TOTAL_BYTES, 1U);
        check_not_null(slices);
        check_not_null(payload);
        check_not_null(received);
        for (size_t i = 0U; i < SENDV_TOTAL_BYTES; ++i) {
            payload[i] = (char)('A' + (i % 23U));
        }
        for (size_t i = 0U; i < SENDV_SLICE_COUNT; ++i) {
            slices[i].data = payload + i * SENDV_SLICE_BYTES;
            slices[i].len = SENDV_SLICE_BYTES;
        }

        memset(&state, 0, sizeof(state));
        state.ctx = ctx;
        state.slices = slices;
        state.slice_count = SENDV_SLICE_COUNT;
        state.received = received;
        state.received_capacity = SENDV_TOTAL_BYTES;
        state.expected_len = SENDV_TOTAL_BYTES;
        state.port = stream_test_pick_loopback_port();
        check_int_gt(state.port, 0);
        server = coro_socket_create_tcpv4(ctx);
        check_not_null(server);
        check_int_eq(coro_socket_listen_on(server, "127.0.0.1", state.port,
                                           stream_sendv_server_handler, &state), 0);
        check_int_eq(coro_context_spawn(ctx, stream_sendv_client_task, &state), 0);
        stream_test_run_while(ctx, stream_sendv_pending, &state, 5000U);
        check_int_eq(state.client_rc, 0);
        check_int_eq(state.handler_rc, 0);
        check_true(state.client_done);
        check_true(state.handler_done);
        check_size_eq(state.received_len, SENDV_TOTAL_BYTES);
        check_mem_eq(state.received, payload, SENDV_TOTAL_BYTES);

        coro_socket_destroy(server);
        stream_test_destroy_context_robust(ctx);
        free(received);
        free(payload);
        free(slices);
    }
#endif

    it("should send and receive compressed frames") {
        coro_context_t *ctx = coro_context_create(NULL);
        stream_zstd_echo_state_t state;
        unsigned short port;
        coro_socket_t *server = NULL;

        check(ctx != NULL);

        memset(&state, 0, sizeof(state));
        state.ctx = ctx;
        port = stream_test_pick_loopback_port();
        check_int_gt(port, 0);
        state.port = port;

        server = coro_socket_create_tcpv4(ctx);
        check_not_null(server);
        check_int_eq(
            coro_socket_listen_on(server, "127.0.0.1", state.port, stream_zstd_echo_server_handler, &state),
            0);

        check_int_eq(coro_context_spawn(ctx, stream_zstd_echo_client_task, &state), 0);
        stream_test_run_while(ctx, stream_test_zstd_echo_pending, &state, 5000);

        check_int_eq(state.handler_done, 1);
        check_int_eq(state.client_done, 1);
        check_int_eq(state.zstd_level_set_rc, 0);
        check_int_eq(state.zstd_disabled_send_rc, TURBO_ENOTSUP);
        check_int_eq(state.client_rc, 0);
        check_int_eq(state.client_recv_rc, 0);
        check_int_eq(state.handler_rc, 0);
        check_str_eq(state.recv_data, g_zstd_payload);

        if (server != NULL) {
            coro_socket_destroy(server);
        }
        coro_context_destroy(ctx);
    }

    it("should auto-compress in send/recv when compression level is set") {
        coro_context_t *ctx = coro_context_create(NULL);
        stream_zstd_auto_echo_state_t state;
        unsigned short port;
        coro_socket_t *server = NULL;

        check(ctx != NULL);

        memset(&state, 0, sizeof(state));
        state.ctx = ctx;
        port = stream_test_pick_loopback_port();
        check_int_gt(port, 0);
        state.port = port;

        server = coro_socket_create_tcpv4(ctx);
        check_not_null(server);
        check_int_eq(
            coro_socket_listen_on(server, "127.0.0.1", state.port, stream_zstd_auto_echo_server_handler, &state),
            0);

        check_int_eq(coro_context_spawn(ctx, stream_zstd_auto_echo_client_task, &state), 0);
        stream_test_run_while(ctx, stream_zstd_auto_echo_pending, &state, 5000);

        check_int_eq(state.handler_done, 1);
        check_int_eq(state.client_done, 1);
        check_int_eq(state.client_set_level_rc, 0);
        check_int_eq(state.client_rc, 0);
        check_int_eq(state.handler_rc, 0);
        check_str_eq(state.recv_data, g_zstd_payload);

        if (server != NULL) {
            coro_socket_destroy(server);
        }
        coro_context_destroy(ctx);
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
        unsigned short port;
        int i;
        check(ctx != NULL);

        port = stream_test_pick_loopback_port();
        check_int_gt(port, 0);

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port);

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

#if defined(__linux__)
    it("should not create one epoll worker thread per tcp connection") {
        int thread_growth = -1;
        check_int_eq(stream_test_backend_thread_growth(TURBO_TCP_BACKEND_EPOLL,
                                                       &thread_growth), TURBO_OK);
        check_int_le(thread_growth, 3);
    }

#if defined(TURBO_HAS_IO_URING)
    it("should share one io_uring reactor across tcp connections") {
        int thread_growth = -1;
        check_int_eq(stream_test_backend_thread_growth(TURBO_TCP_BACKEND_IO_URING,
                                                       &thread_growth), TURBO_OK);
        check_int_le(thread_growth, 3);
    }
#endif
#endif

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
