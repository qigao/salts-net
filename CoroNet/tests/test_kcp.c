#include "CoroNet.h"
#include "CoroNet/turbo_coro_internal.h"
#include "CoroNet/turbo_kcp.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

static int s_kcp_recv_count = 0;
static int on_kcp_recv(void *handle, const mem_slice_t *slice, void *peer) {
    (void)handle; (void)peer;
    if (slice && slice->length == 5 && memcmp(slice->data, "world", 5) == 0) {
        s_kcp_recv_count++;
    }
    return 0;
}

static void on_kcp_connect(void *handle, int status, void *peer) {
    (void)handle; (void)status; (void)peer;
}

typedef struct {
    coro_context_t *ctx;
    volatile int client_done;
    volatile int server_done;
    volatile int ok;
} kcp_socket_echo_state_t;

static void kcp_socket_echo_handler(coro_socket_t *client, void *arg) {
    kcp_socket_echo_state_t *state = (kcp_socket_echo_state_t *)arg;
    char *data = NULL;
    size_t len = 0;

    if (coro_socket_recv(client, &data, &len) == 0 && data && len == 4 &&
        memcmp(data, "ping", 4) == 0) {
        if (coro_socket_send(client, "pong", 4) == 0) {
            state->server_done = 1;
        }
    }

    if (data) {
        coro_socket_free_recv(data);
    }
}

static void kcp_socket_echo_client(coro_t *co, void *arg) {
    (void)co;
    kcp_socket_echo_state_t *state = (kcp_socket_echo_state_t *)arg;
    coro_socket_t *client = coro_socket_create_kcp(state->ctx);
    char *data = NULL;
    size_t len = 0;

    if (client) {
        coro_socket_set_timeout(client, 2000);
        if (coro_socket_connect(client, "127.0.0.1", 28652) == 0 &&
            coro_socket_send(client, "ping", 4) == 0 &&
            coro_socket_recv(client, &data, &len) == 0 && data && len == 4 &&
            memcmp(data, "pong", 4) == 0) {
            state->ok = 1;
            state->client_done = 1;
        }
    }

    if (data) {
        coro_socket_free_recv(data);
    }
    if (client) {
        coro_socket_destroy(client);
    }
    coro_context_stop(state->ctx);
}

spec("KCP Transport") {
    it("should create and destroy kcp context") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        turbo_kcp_t *kcp = turbo_kcp_create(ctx);
        check(kcp != NULL);

        turbo_kcp_destroy(kcp);

        coro_context_run(ctx, TURBO_RUN_DEFAULT);
        coro_context_destroy(ctx);
    }

    it("should initiate KCP over UDP") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        turbo_kcp_t *client = turbo_kcp_create(ctx);
        check(client != NULL);

        int r = turbo_kcp_connect(client, "127.0.0.1", 9999, on_kcp_connect, on_kcp_recv);
        check_int_eq(r, 0);

        turbo_kcp_destroy(client);
        
        uint64_t start = coro_context_now(ctx);
        while (coro_context_alive(ctx) && (coro_context_now(ctx) - start < 1000)) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }
        check(!coro_context_alive(ctx));
        coro_context_destroy(ctx);
    }

    it("should propagate invalid udp backend through turbo_kcp_connect") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);
        ctx->udp_backend = (turbo_udp_backend_t)-1;

        turbo_kcp_t *client = turbo_kcp_create(ctx);
        check(client != NULL);

        check_int_eq(turbo_kcp_connect(client, "127.0.0.1", 9999, on_kcp_connect, on_kcp_recv),
                     TURBO_EPROTONOSUPPORT);
        check_int_eq(coro_context_get_last_error(ctx), TURBO_EPROTONOSUPPORT);

        turbo_kcp_destroy(client);
        coro_context_run(ctx, TURBO_RUN_DEFAULT);
        coro_context_destroy(ctx);
    }

    it("should send and receive data over KCP") {
        coro_context_t *ctx = coro_context_create(NULL);
        
        turbo_kcp_t *server = turbo_kcp_create(ctx);
        turbo_kcp_t *client = turbo_kcp_create(ctx);
        
        s_kcp_recv_count = 0;
        
        /* 1. Bind server to dynamic port */
        check_int_eq(turbo_kcp_bind(server, "127.0.0.1", 0, on_kcp_recv), 0);
        
        /* 1.5 Bind client to dynamic loopback port */
        check_int_eq(turbo_kcp_bind(client, "127.0.0.1", 0, on_kcp_recv), 0);
        
        /* 2. Get server port */
        struct sockaddr_storage server_addr;
        turbo_datagram_t *s_dg = turbo_kcp_get_datagram(server);
        check_int_eq(turbo_datagram_get_local_addr(s_dg, &server_addr), 0);
        
        unsigned short port = 0;
        if (server_addr.ss_family == AF_INET) {
            port = ntohs(((struct sockaddr_in*)&server_addr)->sin_port);
        }
        
        /* 3. Connect client to server */
        check_int_eq(turbo_kcp_connect(client, "127.0.0.1", port, on_kcp_connect, on_kcp_recv), 0);
        
        /* 4. Send data: client -> server */
        /* Note: ikcp_send is just putting it in the send queue. We need to run loop to actually output. */
        check_int_eq(turbo_kcp_send(client, "world", 5), 0);
        
        /* 5. Run loop until server receives it */
        uint64_t wait_start = coro_context_now(ctx);
        while (s_kcp_recv_count < 1 && coro_context_alive(ctx) && (coro_context_now(ctx) - wait_start < 2000)) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }
        
        check_int_gt(s_kcp_recv_count, 0);
        
        turbo_kcp_destroy(client);
        turbo_kcp_destroy(server);

        uint64_t start = coro_context_now(ctx);
        while (coro_context_alive(ctx) && (coro_context_now(ctx) - start < 1000)) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }
        check(!coro_context_alive(ctx));
        coro_context_destroy(ctx);
    }

    it("should support KCP server sockets through coro_socket_listen_on") {
        coro_context_t *ctx = coro_context_create(NULL);
        check_not_null(ctx);

        coro_socket_t *server = coro_socket_create_kcp(ctx);
        check_not_null(server);

        kcp_socket_echo_state_t state = {.ctx = ctx};

        check_int_eq(coro_socket_listen_on(server, "127.0.0.1", 28652, kcp_socket_echo_handler, &state), 0);
        check_int_eq(coro_context_spawn(ctx, kcp_socket_echo_client, &state), 0);

        coro_context_run(ctx, TURBO_RUN_DEFAULT);

        check_int_eq(state.client_done, 1);
        check_int_eq(state.server_done, 1);
        check_int_eq(state.ok, 1);

        coro_socket_destroy(server);
        coro_context_destroy(ctx);
    }

    it("should propagate invalid udp backend through kcp socket bind") {
        coro_context_t *ctx = coro_context_create(NULL);
        check_not_null(ctx);
        ctx->udp_backend = (turbo_udp_backend_t)-1;

        coro_socket_t *server = coro_socket_create_kcp(ctx);
        check_not_null(server);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);

        check_int_eq(coro_socket_bind(server, (struct sockaddr *)&addr), TURBO_EPROTONOSUPPORT);
        check_int_eq(coro_context_get_last_error(ctx), TURBO_EPROTONOSUPPORT);

        coro_socket_destroy(server);
        coro_context_run(ctx, TURBO_RUN_DEFAULT);
        coro_context_destroy(ctx);
    }
}
