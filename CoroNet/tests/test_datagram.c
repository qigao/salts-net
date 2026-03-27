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

spec("Datagram") {
#if defined(__linux__) || defined(__ANDROID__)
    it("should reject unavailable io_uring udp backend") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        check_int_eq(coro_context_set_udp_backend(ctx, TURBO_UDP_BACKEND_IO_URING),
                     TURBO_ENOTSUP);

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

        coro_context_run(ctx, TURBO_RUN_DEFAULT);
        coro_context_destroy(ctx);
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
        coro_context_run(ctx, TURBO_RUN_DEFAULT);
        coro_context_destroy(ctx);
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

        // Run the event loop momentarily to process the async send and receive
        coro_context_run(ctx, TURBO_RUN_ONCE);
        // Sometimes it takes another tick
        if (!s_received) coro_context_run(ctx, TURBO_RUN_ONCE);
        if (!s_received) coro_context_run(ctx, TURBO_RUN_ONCE);

        check_int_eq(s_received, 1);

        turbo_datagram_destroy(server);
        turbo_datagram_destroy(client);

        coro_context_run(ctx, TURBO_RUN_DEFAULT); // Clean up handles
        coro_context_destroy(ctx);
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
        }

        /* Run loop until all received or timeout */
        int limit = 1000;
        while (s_stress_received < total && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        check_int_eq(s_stress_received, total);

        turbo_datagram_destroy(server);
        turbo_datagram_destroy(client);
        coro_context_run(ctx, TURBO_RUN_DEFAULT);
        coro_context_destroy(ctx);
    }
}
