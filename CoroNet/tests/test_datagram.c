#include "CoroNet.h"
#include "CoroNet/turbo_coro_internal.h"
#include "turbo_datagram.h"
#include "tinytest.h"
#include <stdio.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

static int s_received = 0;
static char s_datagram_send_payload[60 * 1024];
static int on_datagram_recv(void *handle, const mem_slice_t *slice, void *addr) {
    turbo_datagram_t *d = (turbo_datagram_t *)handle;
    (void)addr;
    if (slice && slice->length > 0) {
        if (slice->length == 5 && memcmp(slice->data, "hello", 5) == 0) {
            s_received = 1;
        }
        turbo_datagram_recv_stop(d);
    }
    return 0;
}

static int s_stress_received = 0;
static int on_stress_recv(void *handle, const mem_slice_t *slice, void *addr) {
    (void)handle; (void)addr;
    if (slice && slice->length > 0) {
        s_stress_received++;
    }
    return 0;
}

static int s_connected_datagram_a_received = 0;
static int s_connected_datagram_b_received = 0;

static int on_connected_datagram_a_recv(void *handle, const mem_slice_t *slice, void *addr) {
    turbo_datagram_t *d = (turbo_datagram_t *)handle;
    (void)addr;
    if (slice && slice->length == 4 && memcmp(slice->data, "pong", 4) == 0) {
        s_connected_datagram_a_received = 1;
        turbo_datagram_recv_stop(d);
    }
    return 0;
}

static int on_connected_datagram_b_recv(void *handle, const mem_slice_t *slice, void *addr) {
    turbo_datagram_t *d = (turbo_datagram_t *)handle;
    (void)addr;
    if (slice && slice->length == 4 && memcmp(slice->data, "ping", 4) == 0) {
        s_connected_datagram_b_received = 1;
        turbo_datagram_recv_stop(d);
    }
    return 0;
}

typedef struct udp_socket_recv_state_s {
    coro_socket_t *socket;
    int rc;
    int done;
    size_t len;
    char payload[16];
    struct sockaddr_storage peer_addr;
} udp_socket_recv_state_t;

typedef struct udp_socket_sendto_state_s {
    coro_socket_t *socket;
    struct sockaddr_storage dest_addr;
    const char *payload;
    size_t len;
    int rc;
    int done;
} udp_socket_sendto_state_t;

static unsigned short datagram_test_port(const struct sockaddr_storage *addr) {
    if (!addr) {
        return 0;
    }

    if (addr->ss_family == AF_INET) {
        return ntohs(((const struct sockaddr_in *)addr)->sin_port);
    }

#ifdef AF_INET6
    if (addr->ss_family == AF_INET6) {
        return ntohs(((const struct sockaddr_in6 *)addr)->sin6_port);
    }
#endif

    return 0;
}

static void datagram_test_make_loopback_addr(struct sockaddr_in *addr, unsigned short port) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr->sin_port = htons(port);
}

static int datagram_test_run_until_all(coro_context_t *ctx, int **predicates, size_t count,
                                       uint64_t timeout_ms) {
    uint64_t deadline;
    size_t i;

    if (!ctx || !predicates || count == 0) {
        return -1;
    }

    deadline = turbo_monotonic_ms() + timeout_ms;
    while (turbo_monotonic_ms() < deadline) {
        int all_done = 1;
        for (i = 0; i < count; i++) {
            if (!predicates[i] || !*predicates[i]) {
                all_done = 0;
                break;
            }
        }
        if (all_done) {
            return 0;
        }
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    for (i = 0; i < count; i++) {
        if (!predicates[i] || !*predicates[i]) {
            return -1;
        }
    }
    return 0;
}

static void udp_socket_recv_task(coro_t *co, void *arg) {
    udp_socket_recv_state_t *state = (udp_socket_recv_state_t *)arg;
    char *data = NULL;
    size_t len = 0;
    int rc;

    (void)co;

    if (!state || !state->socket) {
        return;
    }

    memset(&state->peer_addr, 0, sizeof(state->peer_addr));
    rc = coro_socket_recvfrom(state->socket, &data, &len, &state->peer_addr);
    state->rc = rc;
    if (rc == 0 && data && len < sizeof(state->payload)) {
        memcpy(state->payload, data, len);
        state->payload[len] = '\0';
        state->len = len;
    }
    if (data) {
        coro_socket_free_recv(data);
    }
    state->done = 1;
}

static void udp_socket_sendto_task(coro_t *co, void *arg) {
    udp_socket_sendto_state_t *state = (udp_socket_sendto_state_t *)arg;

    (void)co;

    if (!state || !state->socket || !state->payload || state->len == 0) {
        return;
    }

    state->rc = coro_socket_sendto(state->socket, state->payload, state->len,
                                   (const struct sockaddr *)&state->dest_addr);
    state->done = 1;
}

static int datagram_test_run_until(coro_context_t *ctx, int *predicate, int expected,
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

static void datagram_test_destroy_context_robust(coro_context_t *ctx) {
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

spec("Datagram") {
#if defined(__linux__) || defined(__ANDROID__)
    it("should reject unavailable io_uring udp backend") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);
#if defined(TURBO_HAS_IO_URING)
        check_int_eq(coro_context_set_udp_backend(ctx, TURBO_UDP_BACKEND_IO_URING), 0);
#else
        check_int_eq(coro_context_set_udp_backend(ctx, TURBO_UDP_BACKEND_IO_URING),
                     TURBO_ENOTSUP);
#endif

        coro_context_destroy(ctx);
    }
#endif

#if defined(__linux__) && defined(TURBO_HAS_IO_URING)
    it("should default udp sockets to io_uring on linux") {
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *sock;

        check(ctx != NULL);
        sock = coro_socket_create_udpv4(ctx);
        check(sock != NULL);
        check_int_eq(coro_socket_get_udp_backend(sock), TURBO_UDP_BACKEND_IO_URING);

        coro_socket_destroy(sock);
        coro_context_destroy(ctx);
    }
#endif

    it("should create, bind, and destroy datagram") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        turbo_datagram_t *dg = turbo_datagram_create(ctx, TURBO_DATAGRAM_UDP4);
        check(dg != NULL);

        int r = turbo_datagram_bind(dg, "127.0.0.1", 0);
        check_int_eq(r, 0);

        turbo_datagram_destroy(dg);

        datagram_test_destroy_context_robust(ctx);
    }

    it("should honor reuse_port for udp listener binds") {
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *server1 = NULL;
        coro_socket_t *server2 = NULL;
        struct sockaddr_in addr;
        struct sockaddr_storage local_addr;
        int r;

        check_not_null(ctx);

        server1 = coro_socket_create_udpv4(ctx);
        server2 = coro_socket_create_udpv4(ctx);
        check_not_null(server1);
        check_not_null(server2);

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);

        coro_socket_set_reuse_port(server1, 1);
        coro_socket_set_reuse_port(server2, 1);

        r = coro_socket_bind(server1, (struct sockaddr *)&addr);
        if (r == 0) {
            check_int_eq(coro_socket_get_local_address(server1, &local_addr), 0);
            addr.sin_port = ((const struct sockaddr_in *)&local_addr)->sin_port;
            check_int_eq(coro_socket_bind(server2, (struct sockaddr *)&addr), 0);
        } else {
            check(r != 0);
        }

        coro_socket_destroy(server2);
        coro_socket_destroy(server1);
        datagram_test_destroy_context_robust(ctx);
    }

    it("should record datagram creation errors on the context") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        check(turbo_datagram_create(ctx, (turbo_datagram_kind_t)-1) == NULL);
        check_int_eq(coro_context_get_last_error(ctx), TURBO_EPROTONOSUPPORT);

        turbo_datagram_t *dg = turbo_datagram_create(ctx, TURBO_DATAGRAM_UDP4);
        check(dg != NULL);
        check_int_eq(coro_context_get_last_error(ctx), 0);

        turbo_datagram_destroy(dg);
        datagram_test_destroy_context_robust(ctx);
    }

    it("should fail udp socket creation when the datagram backend is invalid") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);
        ctx->udp_backend = (turbo_udp_backend_t)-1;

        coro_socket_t *sock = coro_socket_create_udpv4(ctx);
        check(sock == NULL);
        check_int_eq(coro_context_get_last_error(ctx), TURBO_EPROTONOSUPPORT);

        coro_context_destroy(ctx);
    }

    it("should send and receive data") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        turbo_datagram_t *server = turbo_datagram_create(ctx, TURBO_DATAGRAM_UDP4);
        turbo_datagram_t *client = turbo_datagram_create(ctx, TURBO_DATAGRAM_UDP4);

        int r = turbo_datagram_bind(server, "127.0.0.1", 0);
        check_int_eq(r, 0);

        r = turbo_datagram_bind(client, "127.0.0.1", 0);
        check_int_eq(r, 0);

        struct sockaddr_storage server_addr;
        r = turbo_datagram_get_local_addr(server, &server_addr);
        check_int_eq(r, 0);

        s_received = 0;
        turbo_datagram_recv_start(server, on_datagram_recv);
        
        r = turbo_datagram_sendto(client, (struct sockaddr*)&server_addr, "hello", 5);
        check_int_eq(r, 0);

        check_int_eq(datagram_test_run_until(ctx, &s_received, 1, 3000), 0);

        turbo_datagram_destroy(server);
        turbo_datagram_destroy(client);

        datagram_test_destroy_context_robust(ctx);
    }

    it("should stress send and receive") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        turbo_datagram_t *server = turbo_datagram_create(ctx, TURBO_DATAGRAM_UDP4);
        turbo_datagram_t *client = turbo_datagram_create(ctx, TURBO_DATAGRAM_UDP4);

        check_int_eq(turbo_datagram_bind(server, "127.0.0.1", 0), 0);
        check_int_eq(turbo_datagram_bind(client, "127.0.0.1", 0), 0);

        struct sockaddr_storage server_addr;
        turbo_datagram_get_local_addr(server, &server_addr);

        s_stress_received = 0;
        turbo_datagram_recv_start(server, on_stress_recv);

        const int total = 100;
        for (int i = 0; i < total; i++) {
            turbo_datagram_sendto(client, (struct sockaddr*)&server_addr, "p", 1);
#ifdef _WIN32
            /* Pace sends on Windows to avoid UDP loopback buffer overflow drops */
            if ((i + 1) % 10 == 0) {
                for(int pump=0; pump<5; pump++) coro_context_run(ctx, TURBO_RUN_NOWAIT);
            }
#endif
        }

        /* Run loop until all received or timeout */
        uint64_t deadline = turbo_monotonic_ms() + 3000;
        while (s_stress_received < total && turbo_monotonic_ms() < deadline) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        check_int_eq(s_stress_received, total);

        turbo_datagram_destroy(server);
        turbo_datagram_destroy(client);
        datagram_test_destroy_context_robust(ctx);
    }

    it("should exchange data on connected datagrams") {
        coro_context_t *ctx = coro_context_create(NULL);
        turbo_datagram_t *left;
        turbo_datagram_t *right;
        struct sockaddr_storage left_addr;
        struct sockaddr_storage right_addr;

        check_not_null(ctx);

        left = turbo_datagram_create(ctx, TURBO_DATAGRAM_UDP4);
        right = turbo_datagram_create(ctx, TURBO_DATAGRAM_UDP4);
        check_not_null(left);
        check_not_null(right);

        check_int_eq(turbo_datagram_bind(left, "127.0.0.1", 0), 0);
        check_int_eq(turbo_datagram_bind(right, "127.0.0.1", 0), 0);
        check_int_eq(turbo_datagram_get_local_addr(left, &left_addr), 0);
        check_int_eq(turbo_datagram_get_local_addr(right, &right_addr), 0);

        check_int_eq(turbo_datagram_connect(left, "127.0.0.1", datagram_test_port(&right_addr)), 0);
        check_int_eq(turbo_datagram_connect(right, "127.0.0.1", datagram_test_port(&left_addr)), 0);

        s_connected_datagram_a_received = 0;
        s_connected_datagram_b_received = 0;
        check_int_eq(turbo_datagram_recv_start(left, on_connected_datagram_a_recv), 0);
        check_int_eq(turbo_datagram_recv_start(right, on_connected_datagram_b_recv), 0);

        check_int_eq(turbo_datagram_send(left, "ping", 4), 0);
        check_int_eq(datagram_test_run_until(ctx, &s_connected_datagram_b_received, 1, 3000), 0);

        check_int_eq(turbo_datagram_send(right, "pong", 4), 0);
        check_int_eq(datagram_test_run_until(ctx, &s_connected_datagram_a_received, 1, 3000), 0);

        turbo_datagram_destroy(left);
        turbo_datagram_destroy(right);
        datagram_test_destroy_context_robust(ctx);
    }

#ifdef _WIN32
    it("should close datagrams with pending recv without use-after-free") {
        enum { DATAGRAM_CLOSE_LOOPS = 16 };
        int i;

        for (i = 0; i < DATAGRAM_CLOSE_LOOPS; ++i) {
            coro_context_t *ctx = coro_context_create(NULL);
            turbo_datagram_t *dg;

            check_not_null(ctx);

            dg = turbo_datagram_create(ctx, TURBO_DATAGRAM_UDP4);
            check_not_null(dg);
            check_int_eq(turbo_datagram_bind(dg, "127.0.0.1", 0), 0);
            check_int_eq(turbo_datagram_recv_start(dg, on_stress_recv), 0);

            turbo_datagram_close(dg);
            turbo_datagram_destroy(dg);
            datagram_test_destroy_context_robust(ctx);
        }
    }

    it("should close datagrams with pending send without use-after-free") {
        enum { DATAGRAM_SEND_CLOSE_LOOPS = 8, DATAGRAM_SEND_BURST = 32 };
        int i;

        memset(s_datagram_send_payload, 'd', sizeof(s_datagram_send_payload));

        for (i = 0; i < DATAGRAM_SEND_CLOSE_LOOPS; ++i) {
            coro_context_t *ctx = coro_context_create(NULL);
            turbo_datagram_t *sender;
            turbo_datagram_t *receiver;
            struct sockaddr_storage receiver_addr;
            int j;

            check_not_null(ctx);

            sender = turbo_datagram_create(ctx, TURBO_DATAGRAM_UDP4);
            receiver = turbo_datagram_create(ctx, TURBO_DATAGRAM_UDP4);
            check_not_null(sender);
            check_not_null(receiver);

            check_int_eq(turbo_datagram_bind(sender, "127.0.0.1", 0), 0);
            check_int_eq(turbo_datagram_bind(receiver, "127.0.0.1", 0), 0);
            check_int_eq(turbo_datagram_get_local_addr(receiver, &receiver_addr), 0);
            check_int_eq(turbo_datagram_connect(sender, "127.0.0.1",
                                                datagram_test_port(&receiver_addr)), 0);

            for (j = 0; j < DATAGRAM_SEND_BURST; ++j) {
                check_int_eq(turbo_datagram_send(sender, s_datagram_send_payload,
                                                 sizeof(s_datagram_send_payload)), 0);
            }

            turbo_datagram_close(sender);
            turbo_datagram_destroy(sender);
            turbo_datagram_destroy(receiver);
            datagram_test_destroy_context_robust(ctx);
        }
    }
#endif

    it("should exchange data on connected coro udp sockets") {
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *left;
        coro_socket_t *right;
        struct sockaddr_in left_bind;
        struct sockaddr_in right_bind;
        struct sockaddr_storage left_addr;
        struct sockaddr_storage right_addr;
        udp_socket_recv_state_t left_recv;
        udp_socket_recv_state_t right_recv;
        int *done_flags[2];

        check_not_null(ctx);

        left = coro_socket_create_udpv4(ctx);
        right = coro_socket_create_udpv4(ctx);
        check_not_null(left);
        check_not_null(right);

        datagram_test_make_loopback_addr(&left_bind, 0);
        datagram_test_make_loopback_addr(&right_bind, 0);
        check_int_eq(coro_socket_bind(left, (struct sockaddr *)&left_bind), 0);
        check_int_eq(coro_socket_bind(right, (struct sockaddr *)&right_bind), 0);
        check_int_eq(coro_socket_get_local_address(left, &left_addr), 0);
        check_int_eq(coro_socket_get_local_address(right, &right_addr), 0);

        check_int_eq(coro_socket_connect(left, "127.0.0.1", datagram_test_port(&right_addr)), 0);
        check_int_eq(coro_socket_connect(right, "127.0.0.1", datagram_test_port(&left_addr)), 0);

        memset(&left_recv, 0, sizeof(left_recv));
        memset(&right_recv, 0, sizeof(right_recv));
        left_recv.socket = left;
        right_recv.socket = right;
        left_recv.rc = -1;
        right_recv.rc = -1;

        check_int_eq(coro_context_spawn(ctx, udp_socket_recv_task, &left_recv), 0);
        check_int_eq(coro_context_spawn(ctx, udp_socket_recv_task, &right_recv), 0);

        /* coro_context_spawn() is lazy; arm both recv waits before sending. */
        for (int i = 0; i < 4; i++) {
            coro_context_run(ctx, TURBO_RUN_NOWAIT);
        }

        check_int_eq(coro_socket_send(left, "ping", 4), 0);
        check_int_eq(coro_socket_send(right, "pong", 4), 0);

        done_flags[0] = &left_recv.done;
        done_flags[1] = &right_recv.done;
        check_int_eq(datagram_test_run_until_all(ctx, done_flags, 2, 3000), 0);

        check_int_eq(left_recv.rc, 0);
        check_int_eq(right_recv.rc, 0);
        check_size_eq(left_recv.len, 4);
        check_size_eq(right_recv.len, 4);
        check_str_eq(left_recv.payload, "pong");
        check_str_eq(right_recv.payload, "ping");
        check_int_eq(datagram_test_port(&left_recv.peer_addr), datagram_test_port(&right_addr));
        check_int_eq(datagram_test_port(&right_recv.peer_addr), datagram_test_port(&left_addr));

        coro_socket_destroy(left);
        coro_socket_destroy(right);
        datagram_test_destroy_context_robust(ctx);
    }

    it("should exchange data on unconnected coro udp sockets with sendto") {
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *left;
        coro_socket_t *right;
        struct sockaddr_in left_bind;
        struct sockaddr_in right_bind;
        struct sockaddr_storage left_addr;
        struct sockaddr_storage right_addr;
        udp_socket_recv_state_t left_recv;
        udp_socket_recv_state_t right_recv;
        udp_socket_sendto_state_t left_send;
        udp_socket_sendto_state_t right_send;
        int *done_flags[4];

        check_not_null(ctx);

        left = coro_socket_create_udpv4(ctx);
        right = coro_socket_create_udpv4(ctx);
        check_not_null(left);
        check_not_null(right);

        datagram_test_make_loopback_addr(&left_bind, 0);
        datagram_test_make_loopback_addr(&right_bind, 0);
        check_int_eq(coro_socket_bind(left, (struct sockaddr *)&left_bind), 0);
        check_int_eq(coro_socket_bind(right, (struct sockaddr *)&right_bind), 0);
        check_int_eq(coro_socket_get_local_address(left, &left_addr), 0);
        check_int_eq(coro_socket_get_local_address(right, &right_addr), 0);

        memset(&left_recv, 0, sizeof(left_recv));
        memset(&right_recv, 0, sizeof(right_recv));
        memset(&left_send, 0, sizeof(left_send));
        memset(&right_send, 0, sizeof(right_send));
        left_recv.socket = left;
        right_recv.socket = right;
        left_recv.rc = -1;
        right_recv.rc = -1;
        left_send.socket = left;
        right_send.socket = right;
        left_send.dest_addr = right_addr;
        right_send.dest_addr = left_addr;
        left_send.payload = "ping";
        right_send.payload = "pong";
        left_send.len = 4;
        right_send.len = 4;
        left_send.rc = -1;
        right_send.rc = -1;

        check_int_eq(coro_context_spawn(ctx, udp_socket_recv_task, &left_recv), 0);
        check_int_eq(coro_context_spawn(ctx, udp_socket_recv_task, &right_recv), 0);
        for (int i = 0; i < 4; i++) {
            coro_context_run(ctx, TURBO_RUN_NOWAIT);
        }

        check_int_eq(coro_context_spawn(ctx, udp_socket_sendto_task, &left_send), 0);
        check_int_eq(coro_context_spawn(ctx, udp_socket_sendto_task, &right_send), 0);

        done_flags[0] = &left_recv.done;
        done_flags[1] = &right_recv.done;
        done_flags[2] = &left_send.done;
        done_flags[3] = &right_send.done;
        check_int_eq(datagram_test_run_until_all(ctx, done_flags, 4, 3000), 0);

        check_int_eq(left_send.rc, 0);
        check_int_eq(right_send.rc, 0);
        check_int_eq(left_recv.rc, 0);
        check_int_eq(right_recv.rc, 0);
        check_size_eq(left_recv.len, 4);
        check_size_eq(right_recv.len, 4);
        check_str_eq(left_recv.payload, "pong");
        check_str_eq(right_recv.payload, "ping");
        check_int_eq(datagram_test_port(&left_recv.peer_addr), datagram_test_port(&right_addr));
        check_int_eq(datagram_test_port(&right_recv.peer_addr), datagram_test_port(&left_addr));

        coro_socket_destroy(left);
        coro_socket_destroy(right);
        datagram_test_destroy_context_robust(ctx);
    }

    it("should receive datagrams after an earlier recvfrom timeout") {
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *left;
        coro_socket_t *right;
        struct sockaddr_in left_bind;
        struct sockaddr_in right_bind;
        struct sockaddr_storage left_addr;
        struct sockaddr_storage right_addr;
        udp_socket_recv_state_t timed_out_recv;
        udp_socket_recv_state_t right_recv;
        udp_socket_sendto_state_t left_send;
        int *done_flags[2];

        check_not_null(ctx);

        left = coro_socket_create_udpv4(ctx);
        right = coro_socket_create_udpv4(ctx);
        check_not_null(left);
        check_not_null(right);

        datagram_test_make_loopback_addr(&left_bind, 0);
        datagram_test_make_loopback_addr(&right_bind, 0);
        check_int_eq(coro_socket_bind(left, (struct sockaddr *)&left_bind), 0);
        check_int_eq(coro_socket_bind(right, (struct sockaddr *)&right_bind), 0);
        check_int_eq(coro_socket_get_local_address(left, &left_addr), 0);
        check_int_eq(coro_socket_get_local_address(right, &right_addr), 0);

        memset(&timed_out_recv, 0, sizeof(timed_out_recv));
        timed_out_recv.socket = right;
        timed_out_recv.rc = -1;
        coro_socket_set_timeout(right, 1);
        check_int_eq(coro_context_spawn(ctx, udp_socket_recv_task, &timed_out_recv), 0);
        check_int_eq(datagram_test_run_until(ctx, &timed_out_recv.done, 1, 3000), 0);
        check_int_eq(timed_out_recv.rc, TURBO_ETIMEDOUT);

        memset(&right_recv, 0, sizeof(right_recv));
        memset(&left_send, 0, sizeof(left_send));
        right_recv.socket = right;
        right_recv.rc = -1;
        left_send.socket = left;
        left_send.dest_addr = right_addr;
        left_send.payload = "late";
        left_send.len = 4;
        left_send.rc = -1;

        coro_socket_set_timeout(right, 3000);
        check_int_eq(coro_context_spawn(ctx, udp_socket_recv_task, &right_recv), 0);
        for (int i = 0; i < 4; i++) {
            coro_context_run(ctx, TURBO_RUN_NOWAIT);
        }
        check_int_eq(coro_context_spawn(ctx, udp_socket_sendto_task, &left_send), 0);

        done_flags[0] = &right_recv.done;
        done_flags[1] = &left_send.done;
        check_int_eq(datagram_test_run_until_all(ctx, done_flags, 2, 3000), 0);

        check_int_eq(left_send.rc, 0);
        check_int_eq(right_recv.rc, 0);
        check_size_eq(right_recv.len, 4);
        check_str_eq(right_recv.payload, "late");
        check_int_eq(datagram_test_port(&right_recv.peer_addr), datagram_test_port(&left_addr));

        coro_socket_destroy(left);
        coro_socket_destroy(right);
        datagram_test_destroy_context_robust(ctx);
    }

    it("should send datagrams after an earlier recvfrom timeout on the sender") {
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *left;
        coro_socket_t *right;
        struct sockaddr_in left_bind;
        struct sockaddr_in right_bind;
        struct sockaddr_storage left_addr;
        struct sockaddr_storage right_addr;
        udp_socket_recv_state_t left_timeout_recv;
        udp_socket_recv_state_t right_recv;
        udp_socket_sendto_state_t left_send;
        int *done_flags[2];

        check_not_null(ctx);

        left = coro_socket_create_udpv4(ctx);
        right = coro_socket_create_udpv4(ctx);
        check_not_null(left);
        check_not_null(right);

        datagram_test_make_loopback_addr(&left_bind, 0);
        datagram_test_make_loopback_addr(&right_bind, 0);
        check_int_eq(coro_socket_bind(left, (struct sockaddr *)&left_bind), 0);
        check_int_eq(coro_socket_bind(right, (struct sockaddr *)&right_bind), 0);
        check_int_eq(coro_socket_get_local_address(left, &left_addr), 0);
        check_int_eq(coro_socket_get_local_address(right, &right_addr), 0);

        memset(&left_timeout_recv, 0, sizeof(left_timeout_recv));
        left_timeout_recv.socket = left;
        left_timeout_recv.rc = -1;
        coro_socket_set_timeout(left, 1);
        check_int_eq(coro_context_spawn(ctx, udp_socket_recv_task, &left_timeout_recv), 0);
        check_int_eq(datagram_test_run_until(ctx, &left_timeout_recv.done, 1, 3000), 0);
        check_int_eq(left_timeout_recv.rc, TURBO_ETIMEDOUT);

        memset(&right_recv, 0, sizeof(right_recv));
        memset(&left_send, 0, sizeof(left_send));
        right_recv.socket = right;
        right_recv.rc = -1;
        left_send.socket = left;
        left_send.dest_addr = right_addr;
        left_send.payload = "send";
        left_send.len = 4;
        left_send.rc = -1;

        coro_socket_set_timeout(right, 3000);
        check_int_eq(coro_context_spawn(ctx, udp_socket_recv_task, &right_recv), 0);
        for (int i = 0; i < 4; i++) {
            coro_context_run(ctx, TURBO_RUN_NOWAIT);
        }
        check_int_eq(coro_context_spawn(ctx, udp_socket_sendto_task, &left_send), 0);

        done_flags[0] = &right_recv.done;
        done_flags[1] = &left_send.done;
        check_int_eq(datagram_test_run_until_all(ctx, done_flags, 2, 3000), 0);

        check_int_eq(left_send.rc, 0);
        check_int_eq(right_recv.rc, 0);
        check_size_eq(right_recv.len, 4);
        check_str_eq(right_recv.payload, "send");
        check_int_eq(datagram_test_port(&right_recv.peer_addr), datagram_test_port(&left_addr));

        coro_socket_destroy(left);
        coro_socket_destroy(right);
        datagram_test_destroy_context_robust(ctx);
    }
}
