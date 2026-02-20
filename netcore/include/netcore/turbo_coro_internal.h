/**
 * @file turbo_coro_internal.h
 * @brief Internal structures for coroutine-based networking.
 *
 * Transport vtable eliminates protocol-specific branching.
 * Each transport (TCP, TLS, KCP, UDP) provides one ops struct.
 */

#ifndef TURBO_CORO_INTERNAL_H
#define TURBO_CORO_INTERNAL_H
#include "turbo_coro.h"
#include "turbo_coro_client.h"
#include "turbo_coro_context.h"
#include <uv.h>
#include <stdint.h>

/* Internal accessor — extract raw loop pointer (NetCore-internal only) */
uv_loop_t* turbo_coro_context_loop(turbo_coro_context_t* ctx);

#include "turbo_tls.h"
#include "turbo_kcp.h"
#include "turbo_udp.h"
#include "turbo_url.h"
#include "turbo_dns.h"
#include "turbo_websocket_client.h"
#include "turbo_websocket_server.h"

typedef struct turbo_coro_transport_ops_s turbo_coro_transport_ops_t;

struct turbo_coro_transport_ops_s {
    int  (*connect)(turbo_coro_client_t* c, const char* host, int port);
    int  (*send)(turbo_coro_client_t* c, const char* data, size_t len);
    int  (*recv_start)(turbo_coro_client_t* c);
    void (*recv_stop)(turbo_coro_client_t* c);
    void (*close)(turbo_coro_client_t* c);
};

struct turbo_coro_client_s {
    uv_loop_t* loop;
    turbo_coro_context_t* ctx;
    turbo_transport_t transport;
    const turbo_coro_transport_ops_t* ops;

    /* Transport handles — only one active at a time */
    union {
        uv_tcp_t tcp;
        turbo_kcp_client_t kcp;
    } handle;
    turbo_tls_client_t* tls;
    turbo_tls_context_t tls_ctx;
    turbo_udp_t udp;

    /* WebSocket (heap-allocated — WS manages its own TCP/TLS internally) */
    turbo_websocket_client_t* ws;
    turbo_websocket_connection_t* ws_conn;
    int ws_is_tls;
    char ws_path[1024];

    int connected;
    int status;
    int tls_cb_fired;

    turbo_coro_t* co_wait;
    char* recv_data;
    size_t recv_len;

    /* UDP peer address for recvfrom */
    struct sockaddr_storage peer_addr;

    /* DNS */
    char resolved_ip[64];
    turbo_dns_query_t* dns_query;
    int dns_initialized;

    /* Timeout */
    uv_timer_t timer;
    uint64_t timeout_ms;
    int timed_out;

    /* Lifecycle */
    int ref_count;

    /* User data */
    void* user_data;
};

/* Transport ops table — defined in turbo_coro_client.c, indexed by turbo_transport_t */
extern const turbo_coro_transport_ops_t* transport_ops_table[];

/* Server-side WS ops — defined in turbo_coro_client.c, used by turbo_coro_server.c */
extern const turbo_coro_transport_ops_t ws_server_ops;

#endif // TURBO_CORO_INTERNAL_H
