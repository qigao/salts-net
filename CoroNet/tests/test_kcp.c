#include "CoroNet.h"
#include "CoroNet/turbo_coro_internal.h"
#include "CoroNet/turbo_kcp.h"
#include "../src/turbo_kcp_fec_internal.h"
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
    int count;
    char payloads[8][32];
    size_t lens[8];
} fec_deliver_capture_t;

static int on_fec_deliver(void *user, const char *data, size_t len) {
    fec_deliver_capture_t *cap = (fec_deliver_capture_t *)user;
    if (!cap || !data || len == 0 || cap->count >= 8 || len >= sizeof(cap->payloads[0])) {
        return TURBO_EINVAL;
    }

    memcpy(cap->payloads[cap->count], data, len);
    cap->payloads[cap->count][len] = '\0';
    cap->lens[cap->count] = len;
    cap->count++;
    return 0;
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

static void kcp_test_destroy_context_robust(coro_context_t *ctx) {
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

spec("KCP Transport") {
    it("should create and destroy kcp context") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        turbo_kcp_t *kcp = turbo_kcp_create(ctx);
        check(kcp != NULL);

        turbo_kcp_destroy(kcp);

        kcp_test_destroy_context_robust(ctx);
    }

    it("should keep KCP FEC disabled by default") {
        coro_context_t *ctx = coro_context_create(NULL);
        turbo_kcp_fec_config_t cfg;
        turbo_kcp_t *kcp;

        check(ctx != NULL);
        kcp = turbo_kcp_create(ctx);
        check(kcp != NULL);

        check_int_eq(turbo_kcp_get_fec(kcp, &cfg), 0);
        check_int_eq(cfg.enabled, 0);
        check_int_eq(cfg.backend, TURBO_KCP_FEC_BACKEND_NONE);
        check_int_eq(cfg.data_shards, 8);
        check_int_eq(cfg.parity_shards, 2);
        check_int_eq(cfg.max_payload_size, 1200);

        turbo_kcp_destroy(kcp);
        kcp_test_destroy_context_robust(ctx);
    }

    it("should validate KCP FEC configuration before enabling") {
        coro_context_t *ctx = coro_context_create(NULL);
        turbo_kcp_fec_config_t cfg;
        turbo_kcp_t *kcp;

        check(ctx != NULL);
        kcp = turbo_kcp_create(ctx);
        check(kcp != NULL);

        turbo_kcp_fec_config_default(&cfg);
        cfg.enabled = 1;
        cfg.backend = TURBO_KCP_FEC_BACKEND_NONE;
        check_int_eq(turbo_kcp_set_fec(kcp, &cfg), TURBO_EINVAL);
        check_int_eq(coro_context_get_last_error(ctx), TURBO_EINVAL);

        turbo_kcp_fec_config_default(&cfg);
        cfg.enabled = 1;
        cfg.backend = TURBO_KCP_FEC_BACKEND_REED_SOLOMON;
        check_int_eq(turbo_kcp_fec_backend_available(TURBO_KCP_FEC_BACKEND_REED_SOLOMON), 1);
        check_int_eq(turbo_kcp_set_fec(kcp, &cfg), 0);

        turbo_kcp_destroy(kcp);
        kcp_test_destroy_context_robust(ctx);
    }

    it("should recover a missing data shard through Reed-Solomon FEC") {
        turbo_kcp_fec_config_t cfg;
        turbo_kcp_fec_state_t *fec;
        mem_buffer_t *data_frame = NULL;
        mem_buffer_t *parity_frame = NULL;
        mem_slice_t slice;
        const char *packets[2] = { "alpha", "bravo" };
        const size_t packet_lens[2] = { 5, 5 };
        fec_deliver_capture_t cap;

        memset(&cap, 0, sizeof(cap));
        turbo_kcp_fec_config_default(&cfg);
        cfg.enabled = 1;
        cfg.backend = TURBO_KCP_FEC_BACKEND_REED_SOLOMON;
        cfg.data_shards = 2;
        cfg.parity_shards = 1;
        cfg.max_payload_size = 32;

        check_int_eq(turbo_kcp_fec_open(&cfg, &fec), 0);
        check_not_null(fec);
        check_int_eq(turbo_kcp_fec_build_data_frame_for_test(&cfg, 7, 1, packets[1], packet_lens[1], &data_frame), 0);
        check_int_eq(
            turbo_kcp_fec_build_reed_solomon_parity_frame_for_test(&cfg, 7, 0, packets, packet_lens, 2, &parity_frame), 0);

        memset(&slice, 0, sizeof(slice));
        slice.data = data_frame->data;
        slice.length = data_frame->used;
        slice.buffer = data_frame;
        check_int_eq(turbo_kcp_fec_receive_frame(fec, &slice, on_fec_deliver, &cap), 0);
        check_int_eq(cap.count, 1);
        check_size_eq(cap.lens[0], 5);
        check_str_eq(cap.payloads[0], "bravo");

        memset(&slice, 0, sizeof(slice));
        slice.data = parity_frame->data;
        slice.length = parity_frame->used;
        slice.buffer = parity_frame;
        check_int_eq(turbo_kcp_fec_receive_frame(fec, &slice, on_fec_deliver, &cap), 0);
        check_int_eq(cap.count, 2);
        check_size_eq(cap.lens[1], 5);
        check_str_eq(cap.payloads[1], "alpha");

        mem_unref(parity_frame);
        mem_unref(data_frame);
        turbo_kcp_fec_close(fec);
    }

    it("should recover two missing data shards from a 5+3 group") {
        turbo_kcp_fec_config_t cfg;
        turbo_kcp_fec_state_t *fec = NULL;
        mem_buffer_t *frames[5] = { NULL, NULL, NULL, NULL, NULL };
        mem_slice_t slice;
        const char *packets[5] = { "zero", "one", "two", "three", "four" };
        const size_t packet_lens[5] = { 4, 3, 3, 5, 4 };
        fec_deliver_capture_t cap;
        int i;

        memset(&cap, 0, sizeof(cap));
        turbo_kcp_fec_config_default(&cfg);
        cfg.enabled = 1;
        cfg.backend = TURBO_KCP_FEC_BACKEND_REED_SOLOMON;
        cfg.data_shards = 5;
        cfg.parity_shards = 3;
        cfg.max_payload_size = 32;

        check_int_eq(turbo_kcp_fec_open(&cfg, &fec), 0);
        check_not_null(fec);
        for (i = 0; i < 3; ++i) {
            check_int_eq(turbo_kcp_fec_build_data_frame_for_test(
                             &cfg, 11, (uint16_t)(i + 2), packets[i + 2],
                             packet_lens[i + 2], &frames[i]), 0);
        }
        check_int_eq(turbo_kcp_fec_build_reed_solomon_parity_frame_for_test(
                         &cfg, 11, 1, packets, packet_lens, 5, &frames[3]), 0);
        check_int_eq(turbo_kcp_fec_build_reed_solomon_parity_frame_for_test(
                         &cfg, 11, 2, packets, packet_lens, 5, &frames[4]), 0);

        for (i = 0; i < 5; ++i) {
            memset(&slice, 0, sizeof(slice));
            slice.data = frames[i]->data;
            slice.length = frames[i]->used;
            slice.buffer = frames[i];
            check_int_eq(turbo_kcp_fec_receive_frame(fec, &slice,
                                                     on_fec_deliver, &cap), 0);
        }
        check_int_eq(cap.count, 5);
        check_str_eq(cap.payloads[0], "two");
        check_str_eq(cap.payloads[1], "three");
        check_str_eq(cap.payloads[2], "four");
        check_str_eq(cap.payloads[3], "zero");
        check_str_eq(cap.payloads[4], "one");

        for (i = 0; i < 5; ++i) mem_unref(frames[i]);
        turbo_kcp_fec_close(fec);
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
        kcp_test_destroy_context_robust(ctx);
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

    it("should send and receive data over KCP with FEC") {
        coro_context_t *ctx = coro_context_create(NULL);
        turbo_kcp_t *server;
        turbo_kcp_t *client;
        turbo_kcp_fec_config_t fec;
        struct sockaddr_storage server_addr;
        turbo_datagram_t *s_dg;
        unsigned short port = 0;
        uint64_t wait_start;
        uint64_t start;

        check(ctx != NULL);
        server = turbo_kcp_create(ctx);
        client = turbo_kcp_create(ctx);
        check(server != NULL);
        check(client != NULL);

        turbo_kcp_fec_config_default(&fec);
        fec.enabled = 1;
        fec.backend = TURBO_KCP_FEC_BACKEND_REED_SOLOMON;
        check_int_eq(turbo_kcp_set_fec(server, &fec), 0);
        check_int_eq(turbo_kcp_set_fec(client, &fec), 0);

        s_kcp_recv_count = 0;
        check_int_eq(turbo_kcp_bind(server, "127.0.0.1", 0, on_kcp_recv), 0);
        check_int_eq(turbo_kcp_bind(client, "127.0.0.1", 0, on_kcp_recv), 0);

        s_dg = turbo_kcp_get_datagram(server);
        check_int_eq(turbo_datagram_get_local_addr(s_dg, &server_addr), 0);
        if (server_addr.ss_family == AF_INET) {
            port = ntohs(((struct sockaddr_in*)&server_addr)->sin_port);
        }
        check_int_eq(turbo_kcp_connect(client, "127.0.0.1", port, on_kcp_connect, on_kcp_recv), 0);
        check_int_eq(turbo_kcp_send(client, "world", 5), 0);

        wait_start = coro_context_now(ctx);
        while (s_kcp_recv_count < 1 && coro_context_alive(ctx) && (coro_context_now(ctx) - wait_start < 2000)) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }
        check_int_gt(s_kcp_recv_count, 0);

        turbo_kcp_destroy(client);
        turbo_kcp_destroy(server);

        start = coro_context_now(ctx);
        while (coro_context_alive(ctx) && (coro_context_now(ctx) - start < 1000)) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }
        check(!coro_context_alive(ctx));
        coro_context_destroy(ctx);
    }

    it("should honor reuse_port for kcp listener binds") {
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *server1 = NULL;
        coro_socket_t *server2 = NULL;
        struct sockaddr_in addr;
        struct sockaddr_storage local_addr;
        int r;

        check_not_null(ctx);

        server1 = coro_socket_create_kcp(ctx);
        server2 = coro_socket_create_kcp(ctx);
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
        kcp_test_destroy_context_robust(ctx);
    }

    it("should expose pending KCP FEC config through coro_socket") {
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *sock;
        turbo_kcp_fec_config_t cfg;
        turbo_kcp_fec_config_t got;

        check_not_null(ctx);
        sock = coro_socket_create_kcp(ctx);
        check_not_null(sock);

        check_int_eq(coro_socket_get_kcp_fec(sock, &got), 0);
        check_int_eq(got.enabled, 0);
        check_int_eq(got.backend, TURBO_KCP_FEC_BACKEND_NONE);

        turbo_kcp_fec_config_default(&cfg);
        check_int_eq(coro_socket_set_kcp_fec(sock, &cfg), 0);
        check_int_eq(coro_socket_get_kcp_fec(sock, &got), 0);
        check_int_eq(got.enabled, 0);

        turbo_kcp_fec_config_default(&cfg);
        cfg.enabled = 1;
        cfg.backend = TURBO_KCP_FEC_BACKEND_NONE;
        check_int_eq(coro_socket_set_kcp_fec(sock, &cfg), TURBO_EINVAL);

        coro_socket_destroy(sock);
        kcp_test_destroy_context_robust(ctx);
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
        kcp_test_destroy_context_robust(ctx);
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
        kcp_test_destroy_context_robust(ctx);
    }
}
