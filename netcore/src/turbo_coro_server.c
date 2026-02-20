/**
 * @file turbo_coro_server.c
 * @brief Coroutine-based network server with TCP, KCP, and UDP support.
 */

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

#include "turbo_coro_server.h"
#include "turbo_coro_internal.h"
#include "turbo_coro.h"
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

/* ── Server struct ────────────────────────────────────────── */

struct turbo_coro_server_s {
    uv_loop_t* loop;
    turbo_coro_context_t* ctx;
    turbo_transport_t transport;

    union {
        uv_tcp_t tcp;
        turbo_kcp_server_t kcp;
        turbo_udp_t udp;
    } handle;

    turbo_websocket_server_t* ws_server;

    turbo_coro_handler_fn handler;
    void* handler_arg;

    turbo_coro_dgram_handler_fn dgram_handler;
    void* dgram_handler_arg;
};

/* ── Coro bridge for connection-oriented handlers ─────────── */

typedef struct {
    turbo_coro_client_t* client;
    turbo_coro_handler_fn handler;
    void* arg;
} coro_task_arg_t;

static void coro_entry_bridge(turbo_coro_t* co, void* arg) {
    UNUSED(co);
    coro_task_arg_t* task = (coro_task_arg_t*)arg;
    task->handler(task->client, task->arg);
    turbo_coro_client_destroy(task->client);
    free(task);
}

static void spawn_client_coro(turbo_coro_client_t* client,
                               turbo_coro_handler_fn handler, void* arg) {
    coro_task_arg_t* task = malloc(sizeof(coro_task_arg_t));
    if (!task) {
        turbo_coro_client_destroy(client);
        return;
    }
    task->client = client;
    task->handler = handler;
    task->arg = arg;

    turbo_coro_t* co = turbo_coro_create(coro_entry_bridge, task, NULL);
    turbo_coro_resume(co);
}

/* ── TCP accept ───────────────────────────────────────────── */

static void on_tcp_connection(uv_stream_t* server_handle, int status) {
    if (status < 0) return;

    turbo_coro_server_t* server = (turbo_coro_server_t*)server_handle->data;
    turbo_coro_client_t* client = turbo_coro_client_create(server->ctx);
    if (!client) return;

    /* Set up as TCP client */
    uv_tcp_init(server->loop, &client->handle.tcp);
    client->handle.tcp.data = client;
    client->transport = TURBO_TCP;
    client->ops = transport_ops_table[TURBO_TCP];

    if (uv_accept(server_handle, (uv_stream_t*)&client->handle.tcp) == 0) {
        client->connected = 1;
        spawn_client_coro(client, server->handler, server->handler_arg);
    } else {
        turbo_coro_client_destroy(client);
    }
}

/* ── KCP accept ───────────────────────────────────────────── */

static void on_kcp_accept(void* server_handle, void* client_handle, void* peer) {
    UNUSED(peer);
    turbo_kcp_server_t* kcp_server = (turbo_kcp_server_t*)server_handle;
    turbo_kcp_client_t* kcp_client = (turbo_kcp_client_t*)client_handle;

    turbo_coro_server_t* server = (turbo_coro_server_t*)
        ((char*)kcp_server - offsetof(turbo_coro_server_t, handle.kcp));

    turbo_coro_client_t* client = turbo_coro_client_create(server->ctx);
    if (!client) return;

    client->transport = TURBO_KCP;
    client->ops = transport_ops_table[TURBO_KCP];

    /* Copy the KCP client into the coro client's handle union */
    memcpy(&client->handle.kcp, kcp_client, sizeof(turbo_kcp_client_t));
    client->handle.kcp.user_data = client;
    
    /* CRITICAL: Link the ORIGINAL client (in arena) to this coro client
       so that callbacks from turbo_kcp.c (using original) find us. */
    kcp_client->user_data = client;

    client->connected = 1;

    spawn_client_coro(client, server->handler, server->handler_arg);
}

static int on_kcp_server_recv(void* handle, const turbo_arena_slice_t* slice, void* peer) {
    /* peer = turbo_kcp_client_t* — forward to the coro client's recv handler */
    turbo_kcp_client_t* kcp_client = (turbo_kcp_client_t*)peer;
    if (!kcp_client || !kcp_client->user_data) return 0;

    turbo_coro_client_t* client = (turbo_coro_client_t*)kcp_client->user_data;
    if (!client->co_wait) return 0;

    /* Reuse the client-side on_kcp_recv logic via the same callback path */
    if (slice && slice->length > 0) {
        client->recv_data = malloc(slice->length);
        if (client->recv_data) {
            memcpy(client->recv_data, slice->data, slice->length);
            client->recv_len = slice->length;
            client->status = 0;
        } else {
            client->status = UV_ENOMEM;
        }
    } else {
        client->status = UV_EOF;
        client->recv_data = NULL;
        client->recv_len = 0;
    }

    if (client->co_wait) {
        turbo_coro_t* co = client->co_wait;
        client->co_wait = NULL;
        turbo_coro_resume(co);
    }
    return 0;
}

/* ── WebSocket accept ─────────────────────────────────────── */

static void on_ws_server_connection(void* handle, int status, void* peer) {
    turbo_websocket_server_t* ws_server = (turbo_websocket_server_t*)handle;
    turbo_websocket_connection_t* conn = (turbo_websocket_connection_t*)peer;
    turbo_coro_server_t* server = (turbo_coro_server_t*)ws_server->user_data;

    if (status < 0) return;

    turbo_coro_client_t* client = turbo_coro_client_create(server->ctx);
    if (!client) return;

    client->transport = TURBO_WEBSOCKET;
    client->ops = &ws_server_ops;
    client->ws_conn = conn;
    client->connected = 1;
    conn->user_data = client;

    spawn_client_coro(client, server->handler, server->handler_arg);
}

static int on_ws_server_recv(void* handle, const turbo_arena_slice_t* slice, void* peer) {
    UNUSED(handle);
    turbo_websocket_connection_t* conn = (turbo_websocket_connection_t*)peer;
    if (!conn || !conn->user_data) return 0;

    turbo_coro_client_t* client = (turbo_coro_client_t*)conn->user_data;
    if (!client->co_wait) return 0;

    if (slice && slice->length > 0) {
        client->recv_data = malloc(slice->length);
        if (client->recv_data) {
            memcpy(client->recv_data, slice->data, slice->length);
            client->recv_len = slice->length;
            client->status = 0;
        } else {
            client->status = UV_ENOMEM;
        }
    } else {
        client->status = UV_EOF;
        client->recv_data = NULL;
        client->recv_len = 0;
    }

    if (client->co_wait) {
        turbo_coro_t* co = client->co_wait;
        client->co_wait = NULL;
        turbo_coro_resume(co);
    }
    return 0;
}

static void on_ws_server_close(void* handle) {
    turbo_websocket_server_t* ws_server = (turbo_websocket_server_t*)handle;
    UNUSED(ws_server);
}

/* ── UDP datagram handler ─────────────────────────────────── */

typedef struct {
    turbo_coro_server_t* server;
    char* data;
    size_t len;
    struct sockaddr_storage addr;
    turbo_coro_dgram_handler_fn handler;
    void* arg;
} dgram_task_arg_t;

static void dgram_coro_bridge(turbo_coro_t* co, void* arg) {
    UNUSED(co);
    dgram_task_arg_t* task = (dgram_task_arg_t*)arg;
    task->handler(task->server, task->data, task->len,
                  (const struct sockaddr*)&task->addr, task->arg);
    free(task->data);
    free(task);
}

static int on_udp_server_recv(void* handle, const turbo_arena_slice_t* slice, void* peer) {
    turbo_udp_t* udp = (turbo_udp_t*)handle;
    turbo_coro_server_t* server = (turbo_coro_server_t*)
        ((char*)udp - offsetof(turbo_coro_server_t, handle.udp));

    if (!server->dgram_handler) return 0;
    if (!slice || slice->length == 0) return 0;

    dgram_task_arg_t* task = malloc(sizeof(dgram_task_arg_t));
    if (!task) return 0;

    task->data = malloc(slice->length);
    if (!task->data) {
        free(task);
        return 0;
    }

    memcpy(task->data, slice->data, slice->length);
    task->len = slice->length;
    task->server = server;
    task->handler = server->dgram_handler;
    task->arg = server->dgram_handler_arg;

    if (peer) {
        const struct sockaddr* sa = (const struct sockaddr*)peer;
        size_t sa_len = (sa->sa_family == AF_INET6)
            ? sizeof(struct sockaddr_in6)
            : sizeof(struct sockaddr_in);
        memset(&task->addr, 0, sizeof(task->addr));
        memcpy(&task->addr, peer, sa_len);
    } else {
        memset(&task->addr, 0, sizeof(struct sockaddr_storage));
    }

    turbo_coro_t* co = turbo_coro_create(dgram_coro_bridge, task, NULL);
    turbo_coro_resume(co);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════ */

turbo_coro_server_t* turbo_coro_server_create(turbo_coro_context_t* ctx) {
    turbo_coro_server_t* server = calloc(1, sizeof(turbo_coro_server_t));
    if (!server) return NULL;
    server->loop = turbo_coro_context_loop(ctx);
    server->ctx = ctx;
    return server;
}

int turbo_coro_server_listen(turbo_coro_server_t* server, const char* url,
                              turbo_coro_handler_fn handler, void* arg) {
    turbo_address_t addr;
    int r = parse_transport_url(url, &addr);
    if (r != 0) return r;
    if (!addr.valid) return UV_EINVAL;

    server->handler = handler;
    server->handler_arg = arg;
    server->transport = addr.transport;

    struct sockaddr_storage saddr;
    if (strchr(addr.host, ':')) {
        r = uv_ip6_addr(addr.host, addr.port, (struct sockaddr_in6*)&saddr);
    } else {
        r = uv_ip4_addr(addr.host, addr.port, (struct sockaddr_in*)&saddr);
    }

    if (addr.transport == TURBO_TCP) {
        uv_tcp_init(server->loop, &server->handle.tcp);
        server->handle.tcp.data = server;

        if (r != 0) return r;
        r = uv_tcp_bind(&server->handle.tcp, (const struct sockaddr*)&saddr, 0);
        if (r != 0) return r;

        return uv_listen((uv_stream_t*)&server->handle.tcp, 128, on_tcp_connection);
    }

    if (addr.transport == TURBO_KCP) {
        r = turbo_kcp_server_init(&server->handle.kcp, server->loop,
                                  addr.host, (unsigned short)addr.port);
        if (r != 0) return r;

        return turbo_kcp_server_start(&server->handle.kcp, on_kcp_accept, on_kcp_server_recv);
    }

    if (addr.transport == TURBO_WEBSOCKET) {
        int is_tls = (strncmp(url, "wss://", 6) == 0);
        turbo_websocket_server_config_t ws_config = {0};

        server->ws_server = turbo_websocket_server_create(server->loop, is_tls, &ws_config);
        if (!server->ws_server) return UV_ENOMEM;

        turbo_websocket_server_set_callbacks(server->ws_server,
            on_ws_server_connection, on_ws_server_recv, on_ws_server_close);
        server->ws_server->user_data = server;

        return turbo_websocket_server_listen(server->ws_server,
            addr.host, addr.port, 128);
    }

    return UV_EPROTONOSUPPORT;
}

int turbo_coro_server_listen_udp(turbo_coro_server_t* server, const char* url,
                                  turbo_coro_dgram_handler_fn handler, void* arg) {
    turbo_address_t addr;
    int r = parse_transport_url(url, &addr);
    if (r != 0) return r;
    if (!addr.valid) return UV_EINVAL;
    if (addr.transport != TURBO_UDP) return UV_EPROTONOSUPPORT;

    server->transport = TURBO_UDP;
    server->dgram_handler = handler;
    server->dgram_handler_arg = arg;

    r = turbo_udp_server_init(&server->handle.udp, server->loop,
                              addr.host, (unsigned short)addr.port);
    if (r != 0) return r;

    return turbo_udp_server_start(&server->handle.udp, on_udp_server_recv);
}

int turbo_coro_server_sendto(turbo_coro_server_t* server,
                              const char* data, size_t len,
                              const struct sockaddr* addr) {
    if (server->transport != TURBO_UDP) return TURBO_EUNSUPPORTED;
    return turbo_udp_send(&server->handle.udp, addr, data, len);
}

static void on_server_handle_close(uv_handle_t* handle) {
    turbo_coro_server_t* server = (turbo_coro_server_t*)handle->data;
    free(server);
}

void turbo_coro_server_destroy(turbo_coro_server_t* server) {
    if (!server) return;

    if (server->transport == TURBO_TCP) {
        uv_close((uv_handle_t*)&server->handle.tcp, on_server_handle_close);
    } else if (server->transport == TURBO_KCP) {
        turbo_kcp_server_stop(&server->handle.kcp);
        free(server);
    } else if (server->transport == TURBO_UDP) {
        turbo_udp_server_stop(&server->handle.udp);
        free(server);
    } else if (server->transport == TURBO_WEBSOCKET) {
        if (server->ws_server) {
            turbo_websocket_server_destroy(server->ws_server);
            server->ws_server = NULL;
        }
        free(server);
    } else {
        free(server);
    }
}
