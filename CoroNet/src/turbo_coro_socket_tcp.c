/**
 * @file turbo_coro_socket_tcp.c
 * @brief TCP transport implementation for coroutine sockets.
 *
 * DESIGN:
 * - Implements coro_transport_ops_t for TCP/TLS
 * - TLS is layered on top of TCP (same ops, different handle)
 * - Reference counting ensures safe async cleanup
 */

#include "CoroNet/turbo_coro_internal.h"
#include <stdlib.h>
#include <string.h>

/* ── Forward declarations ─────────────────────────────────── */
static void on_tcp_handle_closed(uv_handle_t *handle);
static void on_write_done(turbo_tcp_client_t *client, int status);

/* ── TCP Connect ──────────────────────────────────────────── */

static int on_tcp_coro_recv(void *handle, const mem_slice_t *slice, void *peer) {
  UNUSED(peer);
  turbo_tcp_client_t *tcp = (turbo_tcp_client_t *)handle;
  coro_socket_t *s = (coro_socket_t *)tcp->user_data;
  coro_socket_handle_transport_recv(s, slice);
  return 0;
}

static void on_tcp_coro_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_tcp_client_t *tcp = (turbo_tcp_client_t *)handle;
  coro_socket_t *s = (coro_socket_t *)tcp->user_data;
  coro_socket_handle_transport_connect(s, status);
}

static void on_tcp_coro_close(void *handle) {
  turbo_tcp_client_t *tcp = (turbo_tcp_client_t *)handle;
  coro_socket_t *s = (coro_socket_t *)tcp->user_data;
  if (s) {
    tcp->user_data = NULL;
    coro_socket_handle_transport_close(s);
  }
}

static int tcp_connect(coro_socket_t *s, const char *host, int port) {
  UNUSED(host); /* Already resolved to s->resolved_ip */
  
  retain_client(s);
  int r = turbo_tcp_client_connect(
      s->handle.tcp, 
      s->resolved_ip, 
      (unsigned short)port,
      on_tcp_coro_recv, 
      on_tcp_coro_connect, 
      on_tcp_coro_close
  );
  
  if (r != 0) {
    release_client(s);
    return r;
  }
  
  coro_set_wait(s);
  coro_yield();
  return s->status;
}

/* ── TCP Bind/Listen/Accept ───────────────────────────────── */

static int tcp_bind(coro_socket_t *s, const struct sockaddr *addr) {
  return uv_tcp_bind(&s->handle.tcp->handle, addr, 0);
}

static void on_tcp_listen(uv_stream_t *stream, int status) {
  turbo_tcp_client_t *tcp = (turbo_tcp_client_t *)stream->data;
  coro_socket_t *s = (coro_socket_t *)tcp->user_data;
  if (!s) return;
  
  s->status = status;
  if (s->co_wait) {
    coro_resume_waiter(s);
  } else {
    s->accept_pending = 1;
  }
}

static int tcp_listen(coro_socket_t *s, int backlog) {
  return uv_listen(
      (uv_stream_t *)&s->handle.tcp->handle, 
      backlog, 
      on_tcp_listen
  );
}

static int tcp_accept(coro_socket_t *s, coro_socket_t **accepted) {
  retain_client(s);
  
  /* Check if accept already fired */
  if (s->accept_pending) {
    s->accept_pending = 0;
    if (s->status != 0) {
      int r = s->status;
      release_client(s);
      return r;
    }
  } else {
    /* Wait for connection */
    coro_set_wait(s);
    coro_yield();
    if (s->status != 0) {
      int r = s->status;
      release_client(s);
      return r;
    }
  }
  
  /* Create child socket */
  coro_socket_t *child = coro_socket_create(s->ctx, CORO_SOCKET_TCP_V4);
  if (!child) {
    release_client(s);
    return UV_ENOMEM;
  }
  
  int r = uv_accept(
      (uv_stream_t *)&s->handle.tcp->handle, 
      (uv_stream_t *)&child->handle.tcp->handle
  );
  
  if (r != 0) {
    coro_socket_destroy(child);
    release_client(s);
    return r;
  }
  
  /* Configure child */
  uv_tcp_nodelay(&child->handle.tcp->handle, 1);
  child->handle.tcp->on_recv = on_tcp_coro_recv;
  child->handle.tcp->on_connect = s->handle.tcp->on_connect;
  child->handle.tcp->on_close = on_tcp_coro_close;
  child->handle.tcp->user_data = child;
  child->connected = 1;
  
  /* Libuv handle holds onto its user_data. 
     The initial ref_count from coro_socket_create is enough. */
  
  *accepted = child;
  release_client(s);
  return 0;
}

/* ── TCP Send/Recv ────────────────────────────────────────── */

static void on_write_done(turbo_tcp_client_t *client, int status) {
  coro_socket_t *s = (coro_socket_t *)client->user_data;
  if (!s) return;
  
  s->write_status = status;
  if (s->co_write_wait) {
    coro_resume_co(s->ctx, s->co_write_wait);
    s->co_write_wait = NULL;
    release_client(s); /* Match retain in tcp_send */
  }
}

static int tcp_send(coro_socket_t *s, const char *data, size_t len) {
  if (s->status != 0) return s->status;
  if (!s->handle.tcp) return UV_ENOTCONN;
  
  s->handle.tcp->on_write_complete = on_write_done;
  int r = turbo_tcp_send(s->handle.tcp, data, len);
  if (r != 0) return r;
  
  /* Wait for write completion if queued */
  if (s->handle.tcp->write_in_progress) {
    s->co_write_wait = coro_running();
    retain_client(s); /* Keep alive until on_write_done or close */
    
    /* Mark coroutine as waiting for I/O if scheduler-managed */
    if (coro_is_scheduled(s->co_write_wait)) {
      coro_set_waiting_for_io(s->co_write_wait, 1);
    }
    
    coro_yield();
    return s->write_status;
  }
  
  return 0;
}

static int tcp_recv_start(coro_socket_t *s) {
  return turbo_tcp_read_start(s->handle.tcp);
}

static void tcp_recv_stop(coro_socket_t *s) {
  turbo_tcp_read_stop(s->handle.tcp);
}

/* ── TCP Close ────────────────────────────────────────────── */

static void tcp_close(coro_socket_t *s) {
  turbo_tcp_client_t *tcp = s->handle.tcp;
  if (!tcp || !s->owns_handle) return;
  
  s->handle.tcp = NULL; /* Atomic clear to prevent double-close */
  
  if (!tcp->closing) {
    /* Initiate asynchronous close. 
       Reference counting (retain_client) ensures 's' stays alive until on_tcp_coro_close. */
    retain_client(s);
    tcp->on_close = on_tcp_coro_close;
    turbo_tcp_client_close(tcp);
  }
}

/* ── TCP Get Local Address ────────────────────────────────── */

static int tcp_get_local_addr(coro_socket_t *s, struct sockaddr_storage *addr) {
  int len = sizeof(struct sockaddr_storage);
  return uv_tcp_getsockname(
      &s->handle.tcp->handle, 
      (struct sockaddr *)addr, 
      &len
  );
}

/* ── TCP Transport Ops ────────────────────────────────────── */

const coro_transport_ops_t transport_ops_tcp = {
    .connect = tcp_connect,
    .bind = tcp_bind,
    .listen = tcp_listen,
    .accept = tcp_accept,
    .send = tcp_send,
    .recv_start = tcp_recv_start,
    .recv_stop = tcp_recv_stop,
    .get_local_addr = tcp_get_local_addr,
    .close = tcp_close
};
