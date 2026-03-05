/**
 * @file coro_client.c
 * @brief Coroutine-based network client with transport vtable.
 *
 * Four transports: TCP, TLS, KCP, UDP.
 * Zero if/else branching in public API — all dispatch through ops.
 */

#include "turbo_coro_client.h"
#include "turbo_coro.h"
#include "turbo_coro_internal.h"
#include "turbo_dns.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* ── Forward declarations ─────────────────────────────────── */

static void on_timer_fired(uv_timer_t *handle);

/* ── Ref counting ─────────────────────────────────────────── */

#include "netcore/turbo_coro_server.h"

void retain_client(coro_client_t *client) {
  if (client) client->ref_count++;
}

void release_client(coro_client_t *client) {
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

static void start_timeout_timer(coro_client_t *client) {
  client->timed_out = 0;
  if (client->timeout_ms > 0) {
    retain_client(client);
    uv_timer_start(&client->timer, on_timer_fired, client->timeout_ms, 0);
  }
}

static void stop_timeout_timer(coro_client_t *client) {
  if (client->timeout_ms > 0 && uv_is_active((uv_handle_t *)&client->timer)) {
    uv_timer_stop(&client->timer);
    release_client(client);
  }
}
/* Non-static helper: wake a coro that is blocked in recv with an EOF status.
 * Used by coro_server.c (on_ws_server_close) which cannot see the
 * static helpers retain_client / stop_timeout_timer / release_client. */
void coro_client_wake_eof(coro_client_t *client) {
  if (!client || !client->co_wait) return;
  stop_timeout_timer(client);
  client->connected = 0;
  client->status = UV_EOF;
  client->recv_data = NULL;
  client->recv_len = 0;
  retain_client(client);
  coro_resume_waiter(client);
  release_client(client);
}

static void coro_wait(coro_client_t *client) {
  start_timeout_timer(client);
  coro_set_wait(client);
  coro_yield();
}

static void on_timer_fired(uv_timer_t *handle) {
  coro_client_t *client = (coro_client_t *)handle->data;
  client->timed_out = 1;
  client->status = UV_ETIMEDOUT;

  if (client->co_wait) {
    coro_t *co = client->co_wait;
    client->co_wait = NULL;

    if (client->co_is_scheduled) {
      // Scheduler-managed: clear waiting_for_io flag
      coro_set_waiting_for_io(co, 0);
    } else {
      // Manually-managed: resume immediately
      coro_resume(co);
    }
  }
  release_client(client);
}

static void resume_coro_with_status(coro_client_t *client, int status) {
  if (client->timed_out) return;

  stop_timeout_timer(client);
  client->status = status;

  if (client->co_wait) {
    coro_t *co = client->co_wait;
    client->co_wait = NULL; // Clear first to avoid double-resume

    if (client->co_is_scheduled) {
      // Scheduler-managed: clear waiting_for_io flag so scheduler will resume
      // it on next tick. Do NOT resume here to avoid use-after-free.
      coro_set_waiting_for_io(co, 0);
    } else {
      // Manually-managed: must resume immediately
      if (coro_alive(co) && co != coro_running()) {
        coro_resume(co);
      }
    }
  }
}

/* ── DNS callback (shared by all transports) ──────────────── */

static void on_dns_resolved(const char *hostname, const char *ip, int status, void *user_data) {
  coro_client_t *client = (coro_client_t *)user_data;
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
    coro_t *co = client->co_wait;
    client->co_wait = NULL;

    if (client->co_is_scheduled) {
      // Scheduler-managed: clear waiting_for_io flag
      coro_set_waiting_for_io(co, 0);
    } else {
      // Manually-managed: resume immediately
      if (co != coro_running()) {
        coro_resume(co);
      }
    }
  }
  release_client(client);
}

/* ── Handle close callback (shared) ──────────────────────── */

static void on_handle_close(uv_handle_t *handle) {
  coro_client_t *client = (coro_client_t *)handle->data;
  release_client(client);
}

/* ── Transport Bridge Callbacks ──────────────────────────── */

/*
 * TCP and PIPE pass their own struct pointer as 'handle' to callbacks.
 * We extract the coro_client via user_data set during connect.
 */

int on_transport_recv(void *handle, const turbo_pool_slice_t *slice, void *peer) {
  UNUSED(peer);
  turbo_tcp_client_t *tcp = (turbo_tcp_client_t *)handle;
  coro_client_t *client = (coro_client_t *)tcp->user_data;

  if (!client->co_wait || client->timed_out) {
    /* No coroutine waiting — buffer the data and stop reading.
       The next coro_recv will find recv_data already set and
       return immediately without yielding. */
    if (client->transport == TURBO_TCP && client->handle.tcp) {
      turbo_tcp_read_stop(client->handle.tcp);
    }
    coro_deliver_recv(client, slice);
    return 0;
  }

  stop_timeout_timer(client);

  /* Stop reading after one chunk to match the coro_recv pattern.
     The next coro_recv will call recv_start again. */
  if (client->transport == TURBO_TCP && client->handle.tcp) {
    turbo_tcp_read_stop(client->handle.tcp);
  } else if (client->transport == TURBO_PIPE && client->handle.pipe) {
    turbo_pipe_read_stop(client->handle.pipe);
  }

  coro_deliver_recv(client, slice);
  coro_resume_waiter(client);
  release_client(client);
  return 0;
}

void on_transport_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_tcp_client_t *tcp = (turbo_tcp_client_t *)handle;
  coro_client_t *client = (coro_client_t *)tcp->user_data;

  if (client->timed_out) return;

  client->connected = (status == 0);
  resume_coro_with_status(client, status);
}

void on_transport_close(void *handle) {
  turbo_tcp_client_t *tcp = (turbo_tcp_client_t *)handle;
  coro_client_t *client = (coro_client_t *)tcp->user_data;
  client->handle.tcp = NULL;
  client->connected = 0;

  /* If a coroutine is waiting for data (e.g. recv yielded), wake it
     with EOF so it doesn't hang forever. */
  if (client->co_wait) {
    stop_timeout_timer(client);
    client->status = UV_EOF;
    client->recv_data = NULL;
    client->recv_len = 0;
    /* Resuming may synchronously trigger coro_client_destroy
       (e.g. server echo handler exits → coro_entry_bridge → destroy).
       That destroy does its own release_client, which could free the
       object before we reach our release below.  Guard with a retain. */
    retain_client(client);
    coro_resume_waiter(client);
    release_client(client);
  }

  release_client(client);
}

/* Pipe-specific bridge callbacks (same logic, different handle type) */

int on_pipe_coro_recv(void *handle, const turbo_pool_slice_t *slice, void *peer) {
  UNUSED(peer);
  turbo_pipe_client_t *pipe = (turbo_pipe_client_t *)handle;
  coro_client_t *client = (coro_client_t *)pipe->user_data;

  if (!client->co_wait || client->timed_out) {
    /* No coroutine waiting — buffer the data and stop reading.
       The next coro_recv will find recv_data already set and
       return immediately without yielding. */
    if (client->handle.pipe) {
      turbo_pipe_read_stop(client->handle.pipe);
    }
    coro_deliver_recv(client, slice);
    return 0;
  }

  stop_timeout_timer(client);
  if (client->handle.pipe) {
    turbo_pipe_read_stop(client->handle.pipe);
  }

  coro_deliver_recv(client, slice);
  coro_resume_waiter(client);
  release_client(client);
  return 0;
}

void on_pipe_coro_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_pipe_client_t *pipe = (turbo_pipe_client_t *)handle;
  coro_client_t *client = (coro_client_t *)pipe->user_data;

  if (client->timed_out) return;

  client->connected = (status == 0);
  resume_coro_with_status(client, status);
}

void on_pipe_coro_close(void *handle) {
  turbo_pipe_client_t *pipe = (turbo_pipe_client_t *)handle;
  coro_client_t *client = (coro_client_t *)pipe->user_data;
  client->handle.pipe = NULL;
  client->connected = 0;

  if (client->co_wait) {
    stop_timeout_timer(client);
    client->status = UV_EOF;
    client->recv_data = NULL;
    client->recv_len = 0;
    retain_client(client);
    coro_resume_waiter(client);
    release_client(client);
  }

  release_client(client);
}

/* ═══════════════════════════════════════════════════════════
 *  TCP transport ops
 * ═══════════════════════════════════════════════════════════ */

static int tcp_connect(coro_client_t *c, const char *host, int port) {
  UNUSED(host);

  retain_client(c);
  int r = turbo_tcp_client_connect(c->handle.tcp, c->resolved_ip, (unsigned short)port,
                                   on_transport_recv, on_transport_connect, on_transport_close);
  if (r != 0) {
    release_client(c);
    return r;
  }

  coro_wait(c);
  return c->status;
}

static int tcp_send(coro_client_t *c, const char *data, size_t len) {
  /* turbo_tcp_send uses the internal op pool and handles fragmentation */
  return turbo_tcp_send(c->handle.tcp, data, len);
}

static int tcp_recv_start(coro_client_t *c) {
  int r = turbo_tcp_read_start(c->handle.tcp);
  if (r == UV_EALREADY) return 0; /* already reading — callback will fire */
  return r;
}

static void tcp_recv_stop(coro_client_t *c) { turbo_tcp_read_stop(c->handle.tcp); }

static void tcp_close(coro_client_t *c) {
  if (c->handle.tcp) {
    turbo_tcp_client_close(c->handle.tcp);
  }
}

static int tcp_get_local_addr(coro_client_t *c, struct sockaddr_storage *addr) {
  if (!c->handle.tcp) return UV_EINVAL;
  int len = sizeof(struct sockaddr_storage);
  return uv_tcp_getsockname(&c->handle.tcp->handle, (struct sockaddr *)addr, &len);
}

static const coro_transport_ops_t tcp_ops = {
    .connect = tcp_connect,
    .send = tcp_send,
    .recv_start = tcp_recv_start,
    .recv_stop = tcp_recv_stop,
    .get_local_addr = tcp_get_local_addr,
    .close = tcp_close,
};

/* ═══════════════════════════════════════════════════════════
 *  Pipe transport ops
 *  Pipe is a uv_stream_t — reuses TCP alloc/read callbacks.
 * ═══════════════════════════════════════════════════════════ */

static int pipe_connect(coro_client_t *c, const char *host, int port) {
  UNUSED(port);

  retain_client(c);
  int r = turbo_pipe_client_connect(c->handle.pipe, host, on_pipe_coro_recv, on_pipe_coro_connect,
                                    on_pipe_coro_close);
  if (r != 0) {
    release_client(c);
    return r;
  }

  coro_wait(c);
  return c->status;
}

static int pipe_send(coro_client_t *c, const char *data, size_t len) {
  return turbo_pipe_send(c->handle.pipe, data, len);
}

static int pipe_recv_start(coro_client_t *c) {
  int r = turbo_pipe_read_start(c->handle.pipe);
  if (r == UV_EALREADY) return 0; /* already reading — callback will fire */
  return r;
}

static void pipe_recv_stop(coro_client_t *c) { turbo_pipe_read_stop(c->handle.pipe); }

static void pipe_close(coro_client_t *c) {
  if (c->handle.pipe) {
    turbo_pipe_client_close(c->handle.pipe);
  }
}

static int pipe_get_local_addr(coro_client_t *c, struct sockaddr_storage *addr) {
  if (!c->handle.pipe) return UV_EINVAL;
  size_t len = sizeof(struct sockaddr_storage);
  /* uv_pipe_getsockname uses a buffer for the path. For ICE/sockaddr we'll return ENOSYS
     as Pipes don't have socket addresses in the IP sense. */
  char buf[1024];
  int r = uv_pipe_getsockname(&c->handle.pipe->handle, buf, &len);
  if (r == 0) return UV_ENOSYS;
  return r;
}

static const coro_transport_ops_t pipe_ops = {
    .connect = pipe_connect,
    .send = pipe_send,
    .recv_start = pipe_recv_start,
    .recv_stop = pipe_recv_stop,
    .get_local_addr = pipe_get_local_addr,
    .close = pipe_close,
};

/* ═══════════════════════════════════════════════════════════
 *  TLS transport ops
 * ═══════════════════════════════════════════════════════════ */

static void on_tls_connect(void *handle, int status, void *peer) {
  turbo_tls_client_t *tls = (turbo_tls_client_t *)handle;
  coro_client_t *client = (coro_client_t *)tls->user_data;
  UNUSED(peer);

  if (client->tls_cb_fired || client->timed_out) return;
  client->tls_cb_fired = 1;

  client->connected = (status == 0);
  resume_coro_with_status(client, status);
  release_client(client);
}

static void on_tls_error(turbo_tls_client_t *tls, int status) {
  coro_client_t *client = (coro_client_t *)tls->user_data;

  if (client->tls_cb_fired || client->timed_out) return;
  client->tls_cb_fired = 1;

  client->connected = 0;
  resume_coro_with_status(client, status);
  release_client(client);
}

static void on_tls_close(void *handle) {
  turbo_tls_client_t *tls = (turbo_tls_client_t *)handle;
  coro_client_t *client = (coro_client_t *)tls->user_data;
  client->tls = NULL;
  client->connected = 0;

  if (client->co_wait) {
    stop_timeout_timer(client);
    client->status = UV_EOF;
    client->recv_data = NULL;
    client->recv_len = 0;
    retain_client(client);
    coro_resume_waiter(client);
    release_client(client);
  }

  release_client(client);
}

static int on_tls_recv(void *handle, const turbo_pool_slice_t *slice, void *peer) {
  turbo_tls_client_t *tls = (turbo_tls_client_t *)handle;
  coro_client_t *client = (coro_client_t *)tls->user_data;
  UNUSED(peer);

  if (!client->co_wait) {
    return 0;
  }
  if (client->timed_out) return 0;

  stop_timeout_timer(client);
  turbo_tls_read_stop(client->tls);

  coro_deliver_recv(client, slice);
  coro_resume_waiter(client);
  release_client(client);
  return 0;
}

static int tls_connect(coro_client_t *c, const char *host, int port) {
  turbo_tls_context_init(&c->tls_ctx, TURBO_TLS_CONTEXT_LIB_INIT);
  turbo_tls_context_set_verify_flags(&c->tls_ctx, TURBO_TLS_VERIFY_NONE);

  c->tls = turbo_tls_client_create(c->loop, &c->tls_ctx);
  if (!c->tls) return UV_ENOMEM;

  turbo_tls_client_set_hostname(c->tls, host, strlen(host));
  c->tls->user_data = c;
  c->tls->handshake_done_cb = on_tls_error;
  c->tls_cb_fired = 0;

  retain_client(c);
  int r = turbo_tls_client_connect(c->tls, c->resolved_ip, (unsigned short)port, on_tls_recv,
                                   on_tls_connect, on_tls_close);
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

static int tls_send(coro_client_t *c, const char *data, size_t len) {
  return turbo_tls_send(c->tls, data, len);
}

static int tls_recv_start(coro_client_t *c) {
  return turbo_tls_read_start(c->tls, NULL, on_tls_recv);
}

static void tls_recv_stop(coro_client_t *c) { turbo_tls_read_stop(c->tls); }

static void tls_close(coro_client_t *c) {
  if (c->tls) {
    turbo_tls_read_stop(c->tls);
    retain_client(c);
    turbo_tls_client_close(c->tls);
    /* Don't null c->tls here — on_tls_close callback will do it */
  }
}

static int tls_get_local_addr(coro_client_t *c, struct sockaddr_storage *addr) {
  if (!c->tls) return UV_EINVAL;
  int len = sizeof(struct sockaddr_storage);
  return uv_tcp_getsockname(&c->tls->handle, (struct sockaddr *)addr, &len);
}

static const coro_transport_ops_t tls_ops = {
    .connect = tls_connect,
    .send = tls_send,
    .recv_start = tls_recv_start,
    .recv_stop = tls_recv_stop,
    .get_local_addr = tls_get_local_addr,
    .close = tls_close,
};

/* ═══════════════════════════════════════════════════════════
 *  KCP transport ops
 * ═══════════════════════════════════════════════════════════ */

static void on_kcp_connect(void *handle, int status, void *peer) {
  turbo_kcp_client_t *kcp = (turbo_kcp_client_t *)handle;
  coro_client_t *client = (coro_client_t *)kcp->user_data;
  UNUSED(peer);

  if (client->timed_out) {
    release_client(client);
    return;
  }

  client->connected = (status == 0);
  resume_coro_with_status(client, status);
  release_client(client);
}

static int on_kcp_recv(void *handle, const turbo_pool_slice_t *slice, void *peer) {
  /* KCP delivers: handle = turbo_kcp_server_t*, peer = turbo_kcp_client_t* */
  UNUSED(handle);
  turbo_kcp_client_t *kcp = (turbo_kcp_client_t *)peer;
  coro_client_t *client = (coro_client_t *)kcp->user_data;
  UNUSED(peer);

  if (!client->co_wait) return 0;
  if (client->timed_out) return 0;

  stop_timeout_timer(client);
  coro_deliver_recv(client, slice);
  coro_resume_waiter(client);
  release_client(client);
  return 0;
}

static int kcp_connect(coro_client_t *c, const char *host, int port) {
  UNUSED(host);

  int r = turbo_kcp_client_init(&c->handle.kcp, c->loop);
  if (r != 0) return r;

  c->handle.kcp.user_data = c;

  retain_client(c);
  r = turbo_kcp_client_connect(&c->handle.kcp, c->resolved_ip, (unsigned short)port, on_kcp_connect,
                               on_kcp_recv);
  if (r != 0) {
    release_client(c);
    return r;
  }

  coro_wait(c);
  return c->status;
}

static int kcp_send(coro_client_t *c, const char *data, size_t len) {
  return turbo_kcp_client_send(&c->handle.kcp, data, len);
}

static int kcp_recv_start(coro_client_t *c) {
  UNUSED(c);
  return 0;
}

static void kcp_recv_stop(coro_client_t *c) { UNUSED(c); }

static void kcp_close(coro_client_t *c) {
  c->connected = 0;

  if (c->co_wait) {
    stop_timeout_timer(c);
    c->status = UV_EOF;
    c->recv_data = NULL;
    c->recv_len = 0;
    coro_resume_waiter(c);
  }

  turbo_kcp_client_close(&c->handle.kcp);
}

static int kcp_get_local_addr(coro_client_t *c, struct sockaddr_storage *addr) {
  if (!c->handle.kcp.server || !c->handle.kcp.server->handle) return UV_EINVAL;
  int len = sizeof(struct sockaddr_storage);
  return uv_udp_getsockname(c->handle.kcp.server->handle, (struct sockaddr *)addr, &len);
}

static const coro_transport_ops_t kcp_ops = {
    .connect = kcp_connect,
    .send = kcp_send,
    .recv_start = kcp_recv_start,
    .recv_stop = kcp_recv_stop,
    .get_local_addr = kcp_get_local_addr,
    .close = kcp_close,
};

/* ═══════════════════════════════════════════════════════════
 *  UDP transport ops
 * ═══════════════════════════════════════════════════════════ */

static int on_udp_recv(void *handle, const turbo_pool_slice_t *slice, void *peer) {
  turbo_udp_t *udp = (turbo_udp_t *)handle;
  coro_client_t *client = (coro_client_t *)((char *)udp - offsetof(coro_client_t, udp));

  if (!client->co_wait) return 0;
  if (client->timed_out) return 0;

  stop_timeout_timer(client);

  /* Copy peer address BEFORE stopping — stop may invalidate arena buffers */
  if (peer) {
    const struct sockaddr *sa = (const struct sockaddr *)peer;
    size_t sa_len =
        (sa->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    memset(&client->peer_addr, 0, sizeof(client->peer_addr));
    memcpy(&client->peer_addr, peer, sa_len);
  }

  coro_deliver_recv(client, slice);

  /* Only stop receiving — do NOT call turbo_udp_server_stop here.
     The caller (turbo_udp.c on_udp_recv) still needs to release the
     slice after we return. Full cleanup happens in udp_close. */
  if (client->udp.handle) {
    uv_udp_recv_stop(client->udp.handle);
  }

  coro_resume_waiter(client);
  release_client(client);
  return 0;
}

static int udp_connect(coro_client_t *c, const char *host, int port) {
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

static int udp_send(coro_client_t *c, const char *data, size_t len) {
  return turbo_udp_send(&c->udp, (const struct sockaddr *)&c->peer_addr, data, len);
}

static int udp_recv_start(coro_client_t *c) {
  /* Stop any existing recv first (timeout path doesn't call recv_stop) */
  if (c->udp.handle) {
    uv_udp_recv_stop(c->udp.handle);
  }
  return turbo_udp_server_start(&c->udp, on_udp_recv);
}

static void udp_recv_stop(coro_client_t *c) {
  if (c->udp.handle) {
    uv_udp_recv_stop(c->udp.handle);
  }
}

static void udp_close(coro_client_t *c) {
  c->connected = 0;

  if (c->co_wait) {
    stop_timeout_timer(c);
    c->status = UV_EOF;
    c->recv_data = NULL;
    c->recv_len = 0;
    coro_resume_waiter(c);
  }

  turbo_udp_server_stop(&c->udp);
}

static int udp_get_local_addr(coro_client_t *c, struct sockaddr_storage *addr) {
  if (!c->udp.handle) return UV_EINVAL;
  int len = sizeof(struct sockaddr_storage);
  return uv_udp_getsockname(c->udp.handle, (struct sockaddr *)addr, &len);
}

static const coro_transport_ops_t udp_ops = {
    .connect = udp_connect,
    .send = udp_send,
    .recv_start = udp_recv_start,
    .recv_stop = udp_recv_stop,
    .get_local_addr = udp_get_local_addr,
    .close = udp_close,
};

/* ═══════════════════════════════════════════════════════════
 *  WebSocket client transport ops
 * ═══════════════════════════════════════════════════════════ */

static void on_ws_connect(void *handle, int status, void *peer) {
  turbo_websocket_client_t *ws = (turbo_websocket_client_t *)handle;
  coro_client_t *client = (coro_client_t *)ws->user_data;
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

static int on_ws_recv(void *handle, const turbo_pool_slice_t *slice, void *peer) {
  turbo_websocket_client_t *ws = (turbo_websocket_client_t *)handle;
  coro_client_t *client = (coro_client_t *)ws->user_data;
  UNUSED(peer);

  if (!client) return 0;
  if (!client->co_wait) return 0;
  if (client->timed_out) return 0;

  stop_timeout_timer(client);
  coro_deliver_recv(client, slice);
  coro_resume_waiter(client);
  release_client(client);
  return 0;
}

static void on_ws_close(void *handle) {
  turbo_websocket_client_t *ws = (turbo_websocket_client_t *)handle;
  if (!ws) return;
  coro_client_t *client = (coro_client_t *)ws->user_data;
  /* Detach immediately to make close callback idempotent under re-entry. */
  ws->user_data = NULL;

  if (!client) return;

  client->ws = NULL;
  client->connected = 0;

  if (client->co_wait) {
    stop_timeout_timer(client);
    client->status = UV_EOF;
    client->recv_data = NULL;
    client->recv_len = 0;
    retain_client(client);
    coro_resume_waiter(client);
    release_client(client);
  }

  release_client(client);
}

static int ws_connect(coro_client_t *c, const char *host, int port) {
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

static int ws_send(coro_client_t *c, const char *data, size_t len) {
  return turbo_websocket_client_send(c->ws, data, len);
}

static int ws_recv_start(coro_client_t *c) {
  UNUSED(c);
  return 0;
}

static void ws_recv_stop(coro_client_t *c) { UNUSED(c); }

static void ws_close(coro_client_t *c) {
  if (c->ws) {
    turbo_websocket_client_close(c->ws, 1000, NULL);
  }
}

static int ws_get_local_addr(coro_client_t *c, struct sockaddr_storage *addr) {
  if (!c->ws) return UV_EINVAL;
  /* WS client doesn't expose the handle easily, but we can try to get it if we had access to the
   * underlying transport */
  return UV_ENOSYS;
}

static const coro_transport_ops_t ws_ops = {
    .connect = ws_connect,
    .send = ws_send,
    .recv_start = ws_recv_start,
    .recv_stop = ws_recv_stop,
    .get_local_addr = ws_get_local_addr,
    .close = ws_close,
};

/* ═══════════════════════════════════════════════════════════
 *  WebSocket server-side transport ops
 *  (for coro clients created by WS server accept)
 * ═══════════════════════════════════════════════════════════ */

static int ws_server_send(coro_client_t *c, const char *data, size_t len) {
  return turbo_websocket_server_send(c->ws_conn, data, len);
}

static int ws_server_recv_start(coro_client_t *c) {
  UNUSED(c);
  return 0;
}

static void ws_server_recv_stop(coro_client_t *c) { UNUSED(c); }

static void ws_server_close(coro_client_t *c) {
  /* ws_conn is owned by the WS server layer and may already be invalidated
     by on_ws_server_close(). Only send CLOSE when we still have a live
     connection binding. */
  if (c->ws_conn && c->connected) {
    turbo_websocket_server_close_connection(c->ws_conn, 1000, NULL);
  }
  c->ws_conn = NULL;
  c->connected = 0;
}

const coro_transport_ops_t ws_server_ops = {
    .connect = NULL,
    .send = ws_server_send,
    .recv_start = ws_server_recv_start,
    .recv_stop = ws_server_recv_stop,
    .close = ws_server_close,
};

/* ═══════════════════════════════════════════════════════════
 *  UDP server-side transport ops
 *  (for coro clients created by UDP server datagram receive)
 * ═══════════════════════════════════════════════════════════ */

static int udp_server_send(coro_client_t *c, const char *data, size_t len) {
  coro_server_t *server = (coro_server_t *)c->user_data;
  if (!server) return UV_EINVAL;
  /* Uses the server's UDP handle to send to the datagram sender */
  return coro_server_sendto(server, data, len, (const struct sockaddr *)&c->peer_addr);
}

static int udp_server_recv_start(coro_client_t *c) {
  c->co_wait = NULL; /* Synchronously resolved; caller won't yield */
  if (!c->dgram_consumed) {
    c->dgram_consumed = 1;
  } else {
    /* Second read returns EOF for this connectionless datagram */
    c->status = UV_EOF;
    c->recv_data = NULL;
    c->recv_len = 0;
  }
  return 0;
}

static void udp_server_recv_stop(coro_client_t *c) { UNUSED(c); }

static void udp_server_close(coro_client_t *c) {
  UNUSED(c);
  /* Server owns the UDP handle, nothing to close here. */
}

const coro_transport_ops_t udp_server_ops = {
    .connect = NULL,
    .send = udp_server_send,
    .recv_start = udp_server_recv_start,
    .recv_stop = udp_server_recv_stop,
    .close = udp_server_close,
};

/* ═══════════════════════════════════════════════════════════
 *  Transport selection table — indexed by turbo_transport_t
 * ═══════════════════════════════════════════════════════════ */

const coro_transport_ops_t *transport_ops_table[] = {
    [TURBO_TCP] = &tcp_ops, [TURBO_TLS] = &tls_ops,   [TURBO_KCP] = &kcp_ops,
    [TURBO_UDP] = &udp_ops, [TURBO_PIPE] = &pipe_ops, [TURBO_WEBSOCKET] = &ws_ops,
};

/* ═══════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════ */

coro_client_t *coro_client_create(coro_context_t *ctx) {
  if (!ctx || !ctx->loop) return NULL;
  uv_loop_t *loop = ctx->loop;
  coro_client_t *client = calloc(1, sizeof(coro_client_t));
  if (!client) return NULL;

  client->loop = loop;
  client->ctx = ctx;
  client->ref_count = 1;
  client->status = UV_EALREADY;

  if (uv_timer_init(loop, &client->timer) != 0) {
    free(client);
    return NULL;
  }
  client->timer.data = client;

  return client;
}

void coro_client_set_timeout(coro_client_t *client, uint64_t timeout_ms) {
  if (client) client->timeout_ms = timeout_ms;
}

int coro_client_connect(coro_client_t *client, const char *url) {
  /* Pre-check: detect wss:// before parse_transport_url (both ws:// and wss://
     map to TURBO_WEBSOCKET, so we need this to set ws_is_tls) */
  if (strncmp(url, "wss://", 6) == 0) {
    client->ws_is_tls = 1;
  }

  turbo_address_t addr;
  int r = parse_transport_url(url, &addr);
  if (r != 0) return r;
  if (!addr.valid) return UV_EINVAL;

  if (addr.transport >= TURBO_TRANSPORT_MAX || !transport_ops_table[addr.transport]) {
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
    client->handle.tcp = turbo_tcp_client_create(client->loop);
    if (!client->handle.tcp) return UV_ENOMEM;
    client->handle.tcp->user_data = client;
  }

  /* Pipe: init handle, stash platform path, skip DNS */
  if (addr.transport == TURBO_PIPE) {
    client->handle.pipe = turbo_pipe_client_create(client->loop);
    if (!client->handle.pipe) return UV_ENOMEM;
    client->handle.pipe->user_data = client;

    /* Stash pipe path in ws_path (1024 bytes, unused for pipe) */
    strncpy(client->ws_path, addr.path, sizeof(client->ws_path) - 1);
    client->ws_path[sizeof(client->ws_path) - 1] = '\0';
    return client->ops->connect(client, addr.path, 0);
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

    /* Set wait BEFORE resolving to handle synchronous DNS callbacks */
    coro_set_wait(client);

    r = turbo_dns_resolve_async2(client->loop, addr.host, TURBO_DNS_ANY, on_dns_resolved, client,
                                 &client->dns_query);
    if (r != 0) {
      client->co_wait = NULL;
      release_client(client);
      return r;
    }

    if (client->co_wait) {
      start_timeout_timer(client);
      coro_yield();
    }
    client->dns_query = NULL;

    if (client->timed_out) return UV_ETIMEDOUT;
    if (client->status != 0) return client->status;
  }

  return client->ops->connect(client, addr.host, addr.port);
}

int coro_client_send(coro_client_t *client, const char *data, size_t len) {
  if (!client->ops) return UV_ENOTCONN;
  return client->ops->send(client, data, len);
}

int coro_client_recv(coro_client_t *client, char **data, size_t *len) {
  if (!client->ops) {
    *data = NULL;
    *len = 0;
    return UV_ENOTCONN;
  }

  /* Data already buffered from a callback that fired before we were waiting
     (e.g. uv_read_start delivered data between connect and recv). */
  if (client->recv_data) {
    *data = client->recv_data;
    *len = client->recv_len;
    int st = client->status;
    client->recv_data = NULL;
    client->recv_len = 0;
    return st;
  }

  retain_client(client);

  /* Set wait BEFORE starting read to catch synchronous callbacks from
     buffered transports (e.g. TLS delivers data synchronously). */
  coro_set_wait(client);

  int r = client->ops->recv_start(client);
  if (r != 0) {
    client->co_wait = NULL;
    release_client(client);
    return r;
  }

  /* If co_wait is still set, the callback hasn't fired yet — yield */
  if (client->co_wait) {
    start_timeout_timer(client);
    coro_yield();
  }
  /* Else: callback fired synchronously, co_wait is NULL. data/status is set. */

  /* If timed out, the callback's early-return skipped its release_client,
     so we must release here to balance the retain above. */
  if (client->timed_out) {
    release_client(client);
  }

  *data = client->recv_data;
  *len = client->recv_len;
  client->recv_data = NULL;
  client->recv_len = 0;

  return client->status;
}

/* ── UDP-specific sendto/recvfrom ─────────────────────────── */

int coro_client_sendto(coro_client_t *client, const char *data, size_t len,
                       const struct sockaddr *addr) {
  if (client->transport != TURBO_UDP) return TURBO_EUNSUPPORTED;
  return turbo_udp_send(&client->udp, addr, data, len);
}

int coro_client_recvfrom(coro_client_t *client, char **data, size_t *len,
                         struct sockaddr_storage *addr) {
  if (client->transport != TURBO_UDP) return TURBO_EUNSUPPORTED;

  retain_client(client);

  /* Set wait BEFORE starting read */
  coro_set_wait(client);

  int r = client->ops->recv_start(client);
  if (r != 0) {
    client->co_wait = NULL;
    release_client(client);
    return r;
  }

  if (client->co_wait) {
    start_timeout_timer(client);
    coro_yield();
  }

  /* If timed out, the callback's early-return skipped its release_client,
     so we must release here to balance the retain above. */
  if (client->timed_out) {
    release_client(client);
  }

  *data = client->recv_data;
  *len = client->recv_len;
  client->recv_data = NULL;
  client->recv_len = 0;
  if (addr) {
    memcpy(addr, &client->peer_addr, sizeof(struct sockaddr_storage));
  }

  return client->status;
}

/* ── Sleep ────────────────────────────────────────────────── */

typedef struct {
  coro_t *co;          /* waiting coroutine handle */
  int co_is_scheduled; /* 1 = scheduler-managed, 0 = manual */
  uv_timer_t timer;
  int status;
} sleep_ctx_t;

static void on_sleep_close(uv_handle_t *handle) { free(handle->data); }

static void on_sleep_fired(uv_timer_t *handle) {
  sleep_ctx_t *ctx = (sleep_ctx_t *)handle->data;
  ctx->status = 0;

  coro_t *co = ctx->co;
  ctx->co = NULL; /* prevent double-resume */

  if (ctx->co_is_scheduled) {
    /* Scheduler-managed: clear waiting_for_io so scheduler resumes on
       the next tick. Do NOT call coro_resume directly — the
       scheduler owns the lifetime and may destroy the coro after resume. */
    coro_set_waiting_for_io(co, 0);
  } else {
    /* Manually-managed: resume immediately */
    if (co && co != coro_running()) {
      coro_resume(co);
    }
  }
}

int coro_sleep(coro_context_t *ctx, uint64_t msec) {
  ASSERT_IN_CORO();
  uv_loop_t *loop = ctx ? ctx->loop : NULL;
  sleep_ctx_t *sctx = malloc(sizeof(sleep_ctx_t));
  if (!sctx) return UV_ENOMEM;

  sctx->co = coro_running();
  sctx->co_is_scheduled = coro_is_scheduled(sctx->co);
  sctx->status = -1;

  uv_timer_init(loop, &sctx->timer);
  sctx->timer.data = sctx;
  uv_timer_start(&sctx->timer, on_sleep_fired, msec, 0);

  /* Mark waiting-for-io so scheduler skips us until the timer fires */
  if (sctx->co_is_scheduled) {
    coro_set_waiting_for_io(sctx->co, 1);
  }

  coro_yield();

  int status = sctx->status;
  uv_close((uv_handle_t *)&sctx->timer, on_sleep_close);
  return status;
}

/* ── Destroy ──────────────────────────────────────────────── */

void coro_client_destroy(coro_client_t *client) {
  if (!client) return;

  if (client->dns_query) {
    turbo_dns_cancel(client->dns_query);
    client->dns_query = NULL;
  }

  if (client->dns_initialized) {
    turbo_dns_cleanup();
  }

  if (!uv_is_closing((uv_handle_t *)&client->timer)) {
    uv_timer_stop(&client->timer);
    retain_client(client);
    uv_close((uv_handle_t *)&client->timer, on_handle_close);
  }

  if (client->ops) {
    /* Some transports release one reference asynchronously in their close
       callback. Hold an extra ref so timer-close + transport-close can
       safely race without freeing the object too early. */
    int needs_async_close_ref = 0;
    switch (client->transport) {
    case TURBO_TCP:
      needs_async_close_ref = (client->handle.tcp != NULL);
      break;
    case TURBO_PIPE:
      needs_async_close_ref = (client->handle.pipe != NULL);
      break;
    case TURBO_WEBSOCKET:
      /* Client-side WS close reports via on_ws_close; server-side ws_conn
         is owned externally and does not release via this client layer. */
      needs_async_close_ref = (client->ws != NULL);
      break;
    default:
      needs_async_close_ref = 0;
      break;
    }
    if (needs_async_close_ref) {
      retain_client(client);
    }
    client->ops->close(client);
  }

  release_client(client);
}

int coro_client_get_local_address(coro_client_t *client, struct sockaddr_storage *addr) {
  if (!client || !client->ops || !client->ops->get_local_addr || !addr) return UV_EINVAL;
  return client->ops->get_local_addr(client, addr);
}

void coro_client_set_user_data(coro_client_t *client, void *data) {
  if (client) client->user_data = data;
}

void *coro_client_get_user_data(coro_client_t *client) { return client ? client->user_data : NULL; }

coro_context_t *coro_client_get_context(coro_client_t *client) {
  return client ? client->ctx : NULL;
}

void coro_client_free_recv(void *data) { free(data); }
