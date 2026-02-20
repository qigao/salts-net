/**
 * @file turbo_coro_client.c
 * @brief Coroutine-based network client with transport vtable.
 *
 * Four transports: TCP, TLS, KCP, UDP.
 * Zero if/else branching in public API — all dispatch through ops.
 */

#include "turbo_coro_client.h"
#include "turbo_coro_internal.h"
#include "turbo_coro.h"
#include "turbo_dns.h"
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <uv.h>

/* ── Forward declarations ─────────────────────────────────── */

static void on_timer_fired(uv_timer_t* handle);
static void release_client(turbo_coro_client_t* client);

static const turbo_coro_transport_ops_t tcp_ops;
static const turbo_coro_transport_ops_t tls_ops;
static const turbo_coro_transport_ops_t kcp_ops;
static const turbo_coro_transport_ops_t udp_ops;
static const turbo_coro_transport_ops_t ws_ops;

/* ── Ref counting ─────────────────────────────────────────── */

static void retain_client(turbo_coro_client_t* client) {
    if (client) client->ref_count++;
}

static void release_client(turbo_coro_client_t* client) {
    if (!client) return;
    if (--client->ref_count == 0) {
        if (client->transport == TURBO_TLS && client->tls) {
            turbo_tls_context_destroy(&client->tls_ctx);
        }
        if (client->transport == TURBO_WEBSOCKET && client->ws) {
            client->ws->user_data = NULL;
            turbo_websocket_client_destroy(client->ws);
            client->ws = NULL;
        }
        free(client);
    }
}

/* ── Coro wait / timeout ──────────────────────────────────── */

static void start_timeout_timer(turbo_coro_client_t* client) {
    client->timed_out = 0;
    if (client->timeout_ms > 0) {
        retain_client(client);
        uv_timer_start(&client->timer, on_timer_fired, client->timeout_ms, 0);
    }
}

static void stop_timeout_timer(turbo_coro_client_t* client) {
    if (client->timeout_ms > 0 && uv_is_active((uv_handle_t*)&client->timer)) {
        uv_timer_stop(&client->timer);
        release_client(client);
    }
}

static void coro_wait(turbo_coro_client_t* client) {
    start_timeout_timer(client);
    client->co_wait = turbo_coro_running();
    turbo_coro_yield();
}

static void on_timer_fired(uv_timer_t* handle) {
    turbo_coro_client_t* client = (turbo_coro_client_t*)handle->data;
    client->timed_out = 1;
    client->status = UV_ETIMEDOUT;

    if (client->co_wait) {
        turbo_coro_t* co = client->co_wait;
        client->co_wait = NULL;
        turbo_coro_resume(co);
    }
    release_client(client);
}

static void resume_coro_with_status(turbo_coro_client_t* client, int status) {
    if (client->timed_out) return;

    stop_timeout_timer(client);
    client->status = status;

    if (client->co_wait) {
        turbo_coro_t* co = client->co_wait;
        if (co != turbo_coro_running()) {
            client->co_wait = NULL;
            turbo_coro_resume(co);
        } else {
            client->co_wait = NULL;
        }
    }
}

/* ── DNS callback (shared by all transports) ──────────────── */

static void on_dns_resolved(const char* hostname, const char* ip, int status, void* user_data) {
    turbo_coro_client_t* client = (turbo_coro_client_t*)user_data;
    UNUSED(hostname);
    if (client->timed_out) {
        release_client(client);
        return;
    }

    stop_timeout_timer(client);
    client->status = status;
    if (status == 0 && ip) {
        strncpy(client->resolved_ip, ip, sizeof(client->resolved_ip) - 1);
        client->resolved_ip[sizeof(client->resolved_ip) - 1] = '\0';
    }

    if (client->co_wait) {
        turbo_coro_t* co = client->co_wait;
        if (co != turbo_coro_running()) {
            client->co_wait = NULL;
            turbo_coro_resume(co);
        } else {
            client->co_wait = NULL;
        }
    }
    release_client(client);
}

/* ── Handle close callback (shared) ──────────────────────── */

static void on_handle_close(uv_handle_t* handle) {
    turbo_coro_client_t* client = (turbo_coro_client_t*)handle->data;
    release_client(client);
}

/* ═══════════════════════════════════════════════════════════
 *  TCP transport ops
 * ═══════════════════════════════════════════════════════════ */

static void on_tcp_connect(uv_connect_t* req, int status) {
    turbo_coro_client_t* client = (turbo_coro_client_t*)req->handle->data;
    free(req);

    if (client->timed_out) {
        release_client(client);
        return;
    }

    client->connected = (status == 0);
    resume_coro_with_status(client, status);
    release_client(client);
}

static int tcp_connect(turbo_coro_client_t* c, const char* host, int port) {
    UNUSED(host);
    struct sockaddr_storage addr;
    int r = turbo_dns_parse_address(c->resolved_ip, port, &addr);
    if (r != 0) return r;

    uv_connect_t* req = malloc(sizeof(uv_connect_t));
    if (!req) return UV_ENOMEM;

    retain_client(c);
    r = uv_tcp_connect(req, &c->handle.tcp, (const struct sockaddr*)&addr, on_tcp_connect);
    if (r != 0) {
        free(req);
        release_client(c);
        return r;
    }

    coro_wait(c);
    return c->status;
}

static void on_tcp_write(uv_write_t* req, int status) {
    turbo_coro_client_t* client = (turbo_coro_client_t*)req->handle->data;
    free(req);
    resume_coro_with_status(client, status);
    release_client(client);
}

static int tcp_send(turbo_coro_client_t* c, const char* data, size_t len) {
    uv_buf_t buf = uv_buf_init((char*)data, (unsigned int)len);
    uv_write_t* req = malloc(sizeof(uv_write_t));
    if (!req) return UV_ENOMEM;

    retain_client(c);
    int r = uv_write(req, (uv_stream_t*)&c->handle.tcp, &buf, 1, on_tcp_write);
    if (r != 0) {
        free(req);
        release_client(c);
        return r;
    }

    coro_wait(c);
    return c->status;
}

static void on_tcp_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    UNUSED(handle);
    buf->base = malloc(suggested_size);
    buf->len = (unsigned long)suggested_size;
}

static void on_tcp_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    turbo_coro_client_t* client = (turbo_coro_client_t*)stream->data;

    if (client->timed_out) {
        if (buf->base) free(buf->base);
        return;
    }

    if (nread == 0) {
        if (buf->base) free(buf->base);
        return;
    }

    stop_timeout_timer(client);
    uv_read_stop(stream);

    if (nread > 0) {
        client->recv_data = buf->base;
        client->recv_len = nread;
        client->status = 0;
    } else {
        if (buf->base) free(buf->base);
        client->status = (int)nread;
        client->recv_data = NULL;
        client->recv_len = 0;
    }

    if (client->co_wait) {
        turbo_coro_t* co = client->co_wait;
        /* If callback fires synchronously (on same stack), don't resume self */
        if (co != turbo_coro_running()) {
            client->co_wait = NULL;
            turbo_coro_resume(co);
        } else {
            /* Synchronous completion, just clear the wait handle so caller knows not to yield */
            client->co_wait = NULL;
        }
    }
    release_client(client);
}

static int tcp_recv_start(turbo_coro_client_t* c) {
    return uv_read_start((uv_stream_t*)&c->handle.tcp, on_tcp_alloc, on_tcp_read);
}

static void tcp_recv_stop(turbo_coro_client_t* c) {
    uv_read_stop((uv_stream_t*)&c->handle.tcp);
}

static void tcp_close(turbo_coro_client_t* c) {
    if (!uv_is_closing((uv_handle_t*)&c->handle.tcp)) {
        retain_client(c);
        uv_close((uv_handle_t*)&c->handle.tcp, on_handle_close);
    }
}

static const turbo_coro_transport_ops_t tcp_ops = {
    .connect    = tcp_connect,
    .send       = tcp_send,
    .recv_start = tcp_recv_start,
    .recv_stop  = tcp_recv_stop,
    .close      = tcp_close,
};

/* ═══════════════════════════════════════════════════════════
 *  TLS transport ops
 * ═══════════════════════════════════════════════════════════ */

static void on_tls_connect(void* handle, int status, void* peer) {
    turbo_tls_client_t* tls = (turbo_tls_client_t*)handle;
    turbo_coro_client_t* client = (turbo_coro_client_t*)tls->user_data;
    UNUSED(peer);

    if (client->tls_cb_fired || client->timed_out) return;
    client->tls_cb_fired = 1;

    client->connected = (status == 0);
    resume_coro_with_status(client, status);
    release_client(client);
}

static void on_tls_error(turbo_tls_client_t* tls, int status) {
    turbo_coro_client_t* client = (turbo_coro_client_t*)tls->user_data;

    if (client->tls_cb_fired || client->timed_out) return;
    client->tls_cb_fired = 1;

    client->connected = 0;
    resume_coro_with_status(client, status);
    release_client(client);
}

static void on_tls_close(void* handle) {
    turbo_tls_client_t* tls = (turbo_tls_client_t*)handle;
    turbo_coro_client_t* client = (turbo_coro_client_t*)tls->user_data;
    release_client(client);
}

static int on_tls_recv(void* handle, const turbo_arena_slice_t* slice, void* peer) {
    turbo_tls_client_t* tls = (turbo_tls_client_t*)handle;
    turbo_coro_client_t* client = (turbo_coro_client_t*)tls->user_data;
    UNUSED(peer);

    if (!client->co_wait) {
        return 0;
    }
    if (client->timed_out) return 0;

    stop_timeout_timer(client);
    turbo_tls_read_stop(client->tls);

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

    turbo_coro_t* co = client->co_wait;
    /* If callback fires synchronously (on same stack), don't resume self */
    if (co != turbo_coro_running()) {
        client->co_wait = NULL;
        turbo_coro_resume(co);
    } else {
        /* Synchronous completion, just clear the wait handle so caller knows not to yield */
        client->co_wait = NULL;
    }
    release_client(client);
    return 0;
}

static int tls_connect(turbo_coro_client_t* c, const char* host, int port) {
    turbo_tls_context_init(&c->tls_ctx, TURBO_TLS_CONTEXT_LIB_INIT);
    turbo_tls_context_set_verify_flags(&c->tls_ctx, TURBO_TLS_VERIFY_NONE);

    c->tls = turbo_tls_client_create(c->loop, &c->tls_ctx);
    if (!c->tls) return UV_ENOMEM;

    turbo_tls_client_set_hostname(c->tls, host, strlen(host));
    c->tls->user_data = c;
    c->tls->handshake_done_cb = on_tls_error;
    c->tls_cb_fired = 0;

    retain_client(c);
    int r = turbo_tls_client_connect(c->tls, c->resolved_ip, (unsigned short)port,
                                     on_tls_recv, on_tls_connect, on_tls_close);
    if (r != 0) {
        release_client(c);
        return r;
    }

    coro_wait(c);

    if (c->connected) {
        turbo_tls_read_stop(c->tls);
    }

    return c->status;
}

static int tls_send(turbo_coro_client_t* c, const char* data, size_t len) {
    return turbo_tls_send(c->tls, data, len);
}

static int tls_recv_start(turbo_coro_client_t* c) {
    return turbo_tls_read_start(c->tls, NULL, on_tls_recv);
}

static void tls_recv_stop(turbo_coro_client_t* c) {
    turbo_tls_read_stop(c->tls);
}

static void tls_close(turbo_coro_client_t* c) {
    if (c->tls) {
        turbo_tls_read_stop(c->tls);
        retain_client(c);
        turbo_tls_client_close(c->tls);
    }
}

static const turbo_coro_transport_ops_t tls_ops = {
    .connect    = tls_connect,
    .send       = tls_send,
    .recv_start = tls_recv_start,
    .recv_stop  = tls_recv_stop,
    .close      = tls_close,
};

/* ═══════════════════════════════════════════════════════════
 *  KCP transport ops
 * ═══════════════════════════════════════════════════════════ */

static void on_kcp_connect(void* handle, int status, void* peer) {
    turbo_kcp_client_t* kcp = (turbo_kcp_client_t*)handle;
    turbo_coro_client_t* client = (turbo_coro_client_t*)kcp->user_data;
    UNUSED(peer);

    if (client->timed_out) {
        release_client(client);
        return;
    }

    client->connected = (status == 0);
    resume_coro_with_status(client, status);
    release_client(client);
}

static int on_kcp_recv(void* handle, const turbo_arena_slice_t* slice, void* peer) {
    /* KCP delivers: handle = turbo_kcp_server_t*, peer = turbo_kcp_client_t* */
    UNUSED(handle);
    turbo_kcp_client_t* kcp = (turbo_kcp_client_t*)peer;
    turbo_coro_client_t* client = (turbo_coro_client_t*)kcp->user_data;
    UNUSED(peer);

    if (!client->co_wait) return 0;
    if (client->timed_out) return 0;

    stop_timeout_timer(client);

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

    turbo_coro_t* co = client->co_wait;
    /* If callback fires synchronously (on same stack), don't resume self */
    if (co != turbo_coro_running()) {
        client->co_wait = NULL;
        turbo_coro_resume(co);
    } else {
        /* Synchronous completion, just clear the wait handle so caller knows not to yield */
        client->co_wait = NULL;
    }
    release_client(client);
    return 0;
}

static int kcp_connect(turbo_coro_client_t* c, const char* host, int port) {
    UNUSED(host);

    int r = turbo_kcp_client_init(&c->handle.kcp, c->loop);
    if (r != 0) return r;

    c->handle.kcp.user_data = c;

    retain_client(c);
    r = turbo_kcp_client_connect(&c->handle.kcp, c->resolved_ip, (unsigned short)port,
                                 on_kcp_connect, on_kcp_recv);
    if (r != 0) {
        release_client(c);
        return r;
    }

    coro_wait(c);
    return c->status;
}

static int kcp_send(turbo_coro_client_t* c, const char* data, size_t len) {
    return turbo_kcp_client_send(&c->handle.kcp, data, len);
}

static int kcp_recv_start(turbo_coro_client_t* c) {
    UNUSED(c);
    return 0;
}

static void kcp_recv_stop(turbo_coro_client_t* c) {
    UNUSED(c);
}

static void kcp_close(turbo_coro_client_t* c) {
    turbo_kcp_client_close(&c->handle.kcp);
}

static const turbo_coro_transport_ops_t kcp_ops = {
    .connect    = kcp_connect,
    .send       = kcp_send,
    .recv_start = kcp_recv_start,
    .recv_stop  = kcp_recv_stop,
    .close      = kcp_close,
};

/* ═══════════════════════════════════════════════════════════
 *  UDP transport ops
 * ═══════════════════════════════════════════════════════════ */

static int on_udp_recv(void* handle, const turbo_arena_slice_t* slice, void* peer) {
    turbo_udp_t* udp = (turbo_udp_t*)handle;
    turbo_coro_client_t* client = (turbo_coro_client_t*)
        ((char*)udp - offsetof(turbo_coro_client_t, udp));

    if (!client->co_wait) return 0;
    if (client->timed_out) return 0;

    stop_timeout_timer(client);

    /* Copy data BEFORE stopping — stop may invalidate arena buffers */
    if (peer) {
        const struct sockaddr* sa = (const struct sockaddr*)peer;
        size_t sa_len = (sa->sa_family == AF_INET6)
            ? sizeof(struct sockaddr_in6)
            : sizeof(struct sockaddr_in);
        memset(&client->peer_addr, 0, sizeof(client->peer_addr));
        memcpy(&client->peer_addr, peer, sa_len);
    }

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

    /* Only stop receiving — do NOT call turbo_udp_server_stop here.
       The caller (turbo_udp.c on_udp_recv) still needs to release the
       slice after we return. Full cleanup happens in udp_close. */
    if (client->udp.handle) {
        uv_udp_recv_stop(client->udp.handle);
    }

    turbo_coro_t* co = client->co_wait;
    /* If callback fires synchronously (on same stack), don't resume self */
    if (co != turbo_coro_running()) {
        client->co_wait = NULL;
        turbo_coro_resume(co);
    } else {
        /* Synchronous completion, just clear the wait handle so caller knows not to yield */
        client->co_wait = NULL;
    }
    return 0;
}

static int udp_connect(turbo_coro_client_t* c, const char* host, int port) {
    UNUSED(host);
    int r = turbo_udp_server_init(&c->udp, c->loop, "0.0.0.0", 0);
    if (r != 0) return r;

    /* Store peer address for send */
    struct sockaddr_in addr4;
    r = uv_ip4_addr(c->resolved_ip, port, &addr4);
    if (r != 0) return r;
    memcpy(&c->peer_addr, &addr4, sizeof(addr4));

    c->connected = 1;
    return 0;
}

static int udp_send(turbo_coro_client_t* c, const char* data, size_t len) {
    return turbo_udp_send(&c->udp, (const struct sockaddr*)&c->peer_addr, data, len);
}

static int udp_recv_start(turbo_coro_client_t* c) {
    return turbo_udp_server_start(&c->udp, on_udp_recv);
}

static void udp_recv_stop(turbo_coro_client_t* c) {
    if (c->udp.handle) {
        uv_udp_recv_stop(c->udp.handle);
    }
}

static void udp_close(turbo_coro_client_t* c) {
    turbo_udp_server_stop(&c->udp);
}

static const turbo_coro_transport_ops_t udp_ops = {
    .connect    = udp_connect,
    .send       = udp_send,
    .recv_start = udp_recv_start,
    .recv_stop  = udp_recv_stop,
    .close      = udp_close,
};

/* ═══════════════════════════════════════════════════════════
 *  WebSocket client transport ops
 * ═══════════════════════════════════════════════════════════ */

static void on_ws_connect(void* handle, int status, void* peer) {
    turbo_websocket_client_t* ws = (turbo_websocket_client_t*)handle;
    turbo_coro_client_t* client = (turbo_coro_client_t*)ws->user_data;
    UNUSED(peer);

    if (!client) return;

    if (client->timed_out) {
        release_client(client);
        return;
    }

    client->connected = (status == 0);
    resume_coro_with_status(client, status);
    release_client(client);
}

static int on_ws_recv(void* handle, const turbo_arena_slice_t* slice, void* peer) {
    turbo_websocket_client_t* ws = (turbo_websocket_client_t*)handle;
    turbo_coro_client_t* client = (turbo_coro_client_t*)ws->user_data;
    UNUSED(peer);

    if (!client) return 0;

    if (!client->co_wait) return 0;
    if (client->timed_out) return 0;

    stop_timeout_timer(client);

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

    turbo_coro_t* co = client->co_wait;
    /* If callback fires synchronously (on same stack), don't resume self */
    if (co != turbo_coro_running()) {
        client->co_wait = NULL;
        turbo_coro_resume(co);
    } else {
        /* Synchronous completion, just clear the wait handle so caller knows not to yield */
        client->co_wait = NULL;
    }
    release_client(client);
    return 0;
}

static void on_ws_close(void* handle) {
    turbo_websocket_client_t* ws = (turbo_websocket_client_t*)handle;
    turbo_coro_client_t* client = (turbo_coro_client_t*)ws->user_data;

    if (!client) return;

    if (client->co_wait) {
        client->status = UV_EOF;
        client->connected = 0;
        turbo_coro_t* co = client->co_wait;
        client->co_wait = NULL;
        turbo_coro_resume(co);
    }
    release_client(client);
}

static int ws_connect(turbo_coro_client_t* c, const char* host, int port) {
    turbo_websocket_config_t config = {0};
    config.path = c->ws_path[0] ? c->ws_path : "/";

    if (c->ws_is_tls) {
        turbo_tls_context_init(&c->tls_ctx, TURBO_TLS_CONTEXT_LIB_INIT);
        turbo_tls_context_set_verify_flags(&c->tls_ctx, TURBO_TLS_VERIFY_NONE);
    }

    c->ws = turbo_websocket_client_create(c->loop, c->ws_is_tls, &config);
    if (!c->ws) return UV_ENOMEM;

    if (c->ws_is_tls) {
        turbo_websocket_client_set_tls_context(c->ws, &c->tls_ctx);
    }

    turbo_websocket_client_set_callbacks(c->ws, on_ws_recv, on_ws_connect, on_ws_close);
    c->ws->user_data = c;

    retain_client(c);
    int r = turbo_websocket_client_connect(c->ws, host, port);
    if (r != 0) {
        release_client(c);
        turbo_websocket_client_destroy(c->ws);
        c->ws = NULL;
        return r;
    }

    coro_wait(c);
    return c->status;
}

static int ws_send(turbo_coro_client_t* c, const char* data, size_t len) {
    return turbo_websocket_client_send(c->ws, data, len);
}

static int ws_recv_start(turbo_coro_client_t* c) {
    UNUSED(c);
    return 0;
}

static void ws_recv_stop(turbo_coro_client_t* c) {
    UNUSED(c);
}

static void ws_close(turbo_coro_client_t* c) {
    if (c->ws) {
        turbo_websocket_client_close(c->ws, 1000, NULL);
    }
}

static const turbo_coro_transport_ops_t ws_ops = {
    .connect    = ws_connect,
    .send       = ws_send,
    .recv_start = ws_recv_start,
    .recv_stop  = ws_recv_stop,
    .close      = ws_close,
};

/* ═══════════════════════════════════════════════════════════
 *  WebSocket server-side transport ops
 *  (for coro clients created by WS server accept)
 * ═══════════════════════════════════════════════════════════ */

static int ws_server_send(turbo_coro_client_t* c, const char* data, size_t len) {
    return turbo_websocket_server_send(c->ws_conn, data, len);
}

static int ws_server_recv_start(turbo_coro_client_t* c) {
    UNUSED(c);
    return 0;
}

static void ws_server_recv_stop(turbo_coro_client_t* c) {
    UNUSED(c);
}

static void ws_server_close(turbo_coro_client_t* c) {
    if (c->ws_conn) {
        turbo_websocket_server_close_connection(c->ws_conn, 1000, NULL);
        c->ws_conn = NULL;
    }
}

const turbo_coro_transport_ops_t ws_server_ops = {
    .connect    = NULL,
    .send       = ws_server_send,
    .recv_start = ws_server_recv_start,
    .recv_stop  = ws_server_recv_stop,
    .close      = ws_server_close,
};

/* ═══════════════════════════════════════════════════════════
 *  Transport selection table — indexed by turbo_transport_t
 * ═══════════════════════════════════════════════════════════ */

const turbo_coro_transport_ops_t* transport_ops_table[] = {
    [TURBO_TCP] = &tcp_ops,
    [TURBO_TLS] = &tls_ops,
    [TURBO_KCP] = &kcp_ops,
    [TURBO_UDP] = &udp_ops,
    [TURBO_WEBSOCKET] = &ws_ops,
};

/* ═══════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════ */

turbo_coro_client_t* turbo_coro_client_create(turbo_coro_context_t* ctx) {
    uv_loop_t* loop = turbo_coro_context_loop(ctx);
    turbo_coro_client_t* client = calloc(1, sizeof(turbo_coro_client_t));
    if (!client) return NULL;

    client->loop = loop;
    client->ctx = ctx;
    client->ref_count = 1;
    client->status = UV_EALREADY;

    uv_timer_init(loop, &client->timer);
    client->timer.data = client;

    return client;
}

void turbo_coro_client_set_timeout(turbo_coro_client_t* client, uint64_t timeout_ms) {
    if (client) client->timeout_ms = timeout_ms;
}

int turbo_coro_client_connect(turbo_coro_client_t* client, const char* url) {
    /* Pre-check: detect wss:// before parse_transport_url (both ws:// and wss://
       map to TURBO_WEBSOCKET, so we need this to set ws_is_tls) */
    if (strncmp(url, "wss://", 6) == 0) {
        client->ws_is_tls = 1;
    }

    turbo_address_t addr;
    int r = parse_transport_url(url, &addr);
    if (r != 0) return r;
    if (!addr.valid) return UV_EINVAL;

    if (addr.transport >= TURBO_TRANSPORT_MAX ||
        !transport_ops_table[addr.transport]) {
        return UV_EPROTONOSUPPORT;
    }

    client->transport = addr.transport;
    client->ops = transport_ops_table[addr.transport];

    /* Copy WS path from parsed URL */
    if (addr.transport == TURBO_WEBSOCKET && addr.path[0]) {
        strncpy(client->ws_path, addr.path, sizeof(client->ws_path) - 1);
        client->ws_path[sizeof(client->ws_path) - 1] = '\0';
    }

    /* Init transport-specific handle */
    if (addr.transport == TURBO_TCP || addr.transport == TURBO_TLS) {
        uv_tcp_init(client->loop, &client->handle.tcp);
        client->handle.tcp.data = client;
    }

    /* WebSocket handles its own TCP/TLS + DNS internally — skip DNS resolution,
       pass hostname directly so TLS SNI works correctly */
    if (addr.transport == TURBO_WEBSOCKET) {
        return client->ops->connect(client, addr.host, addr.port);
    }

    /* DNS resolution — skip for literal IPs (the async resolver calls
       the callback synchronously for IPs, which fires before coro_wait) */
    struct sockaddr_storage probe;
    if (turbo_dns_parse_address(addr.host, 0, &probe) == 0) {
        strncpy(client->resolved_ip, addr.host, sizeof(client->resolved_ip) - 1);
        client->resolved_ip[sizeof(client->resolved_ip) - 1] = '\0';
    } else {
        turbo_dns_init();
        client->dns_initialized = 1;

        retain_client(client);
        
        /* Set wait handle BEFORE starting DNS to handle synchronous resolution */
        client->co_wait = turbo_coro_running();

        r = turbo_dns_resolve_async2(client->loop, addr.host, TURBO_DNS_ANY,
                                     on_dns_resolved, client, &client->dns_query);
        if (r != 0) {
            client->co_wait = NULL;
            release_client(client);
            return r;
        }

        if (client->co_wait) {
            start_timeout_timer(client);
            turbo_coro_yield();
        }
        client->dns_query = NULL;

        if (client->timed_out) return UV_ETIMEDOUT;
        if (client->status != 0) return client->status;
    }

    return client->ops->connect(client, addr.host, addr.port);
}

int turbo_coro_client_send(turbo_coro_client_t* client, const char* data, size_t len) {
    return client->ops->send(client, data, len);
}

int turbo_coro_client_recv(turbo_coro_client_t* client, char** data, size_t* len) {
    retain_client(client);
    
    /* Set wait handle BEFORE starting read to catch synchronous callbacks from buffered transports (e.g. TLS) */
    client->co_wait = turbo_coro_running();

    int r = client->ops->recv_start(client);
    if (r != 0) {
        client->co_wait = NULL;
        release_client(client);
        return r;
    }

    /* If co_wait is still set, it means callback hasn't fired yet -> wait */
    if (client->co_wait) {
        start_timeout_timer(client);
        turbo_coro_yield();
    }
    /* Else: callback fired synchronously, co_wait is NULL. Resume failed (expected), but data/status is set. */

    *data = client->recv_data;
    *len = client->recv_len;

    return client->status;
}

/* ── UDP-specific sendto/recvfrom ─────────────────────────── */

int turbo_coro_client_sendto(turbo_coro_client_t* client,
                              const char* data, size_t len,
                              const struct sockaddr* addr) {
    if (client->transport != TURBO_UDP) return TURBO_EUNSUPPORTED;
    return turbo_udp_send(&client->udp, addr, data, len);
}

int turbo_coro_client_recvfrom(turbo_coro_client_t* client,
                                char** data, size_t* len,
                                struct sockaddr_storage* addr) {
    if (client->transport != TURBO_UDP) return TURBO_EUNSUPPORTED;

    retain_client(client);

    /* Set wait handle BEFORE starting read */
    client->co_wait = turbo_coro_running();

    int r = client->ops->recv_start(client);
    if (r != 0) {
        client->co_wait = NULL;
        release_client(client);
        return r;
    }

    if (client->co_wait) {
        start_timeout_timer(client);
        turbo_coro_yield();
    }

    *data = client->recv_data;
    *len = client->recv_len;
    if (addr) {
        memcpy(addr, &client->peer_addr, sizeof(struct sockaddr_storage));
    }

    return client->status;
}

/* ── Sleep ────────────────────────────────────────────────── */

typedef struct {
    turbo_coro_t* co;
    uv_timer_t timer;
    int status;
} sleep_ctx_t;

static void on_sleep_close(uv_handle_t* handle) {
    free(handle->data);
}

static void on_sleep_fired(uv_timer_t* handle) {
    sleep_ctx_t* ctx = (sleep_ctx_t*)handle->data;
    ctx->status = 0;
    turbo_coro_resume(ctx->co);
}

int turbo_coro_sleep(turbo_coro_context_t* ctx, uint64_t msec) {
    uv_loop_t* loop = turbo_coro_context_loop(ctx);
    sleep_ctx_t* sctx = malloc(sizeof(sleep_ctx_t));
    if (!sctx) return UV_ENOMEM;

    sctx->co = turbo_coro_running();
    sctx->status = -1;

    uv_timer_init(loop, &sctx->timer);
    sctx->timer.data = sctx;
    uv_timer_start(&sctx->timer, on_sleep_fired, msec, 0);

    turbo_coro_yield();

    uv_close((uv_handle_t*)&sctx->timer, on_sleep_close);
    return sctx->status;
}

/* ── Destroy ──────────────────────────────────────────────── */

void turbo_coro_client_destroy(turbo_coro_client_t* client) {
    if (!client) return;

    if (client->dns_query) {
        turbo_dns_cancel(client->dns_query);
        client->dns_query = NULL;
    }

    if (client->dns_initialized) {
        turbo_dns_cleanup();
    }

    if (!uv_is_closing((uv_handle_t*)&client->timer)) {
        uv_timer_stop(&client->timer);
        retain_client(client);
        uv_close((uv_handle_t*)&client->timer, on_handle_close);
    }

    if (client->ops) {
        client->ops->close(client);
    }

    release_client(client);
}

void turbo_coro_client_set_user_data(turbo_coro_client_t* client, void* data) {
    if (client) client->user_data = data;
}

void* turbo_coro_client_get_user_data(turbo_coro_client_t* client) {
    return client ? client->user_data : NULL;
}

turbo_coro_context_t* turbo_coro_client_get_context(turbo_coro_client_t* client) {
    return client ? client->ctx : NULL;
}
