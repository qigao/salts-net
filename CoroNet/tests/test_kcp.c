#include "CoroNet.h"
#include "CoroNet/turbo_kcp.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>

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
}
