/**
 * @file turbo_coro_socket_kcp.c
 * @brief KCP (reliable UDP) transport implementation.
 */

#include "CoroNet/turbo_coro_internal.h"
#include <stdlib.h>

/* ── KCP Callbacks ────────────────────────────────────────── */

static int on_kcp_recv(void *handle, const mem_slice_t *slice, void *peer) {
  turbo_kcp_client_t *client = (turbo_kcp_client_t *)peer;
  coro_socket_t *s = (coro_socket_t *)client->user_data;
  if (s) {
    if (s->ops->recv_stop) s->ops->recv_stop(s);
    coro_socket_handle_transport_recv(s, slice);
  }
  return 0;
}

static void on_kcp_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_kcp_client_t *client = (turbo_kcp_client_t *)handle;
  coro_socket_t *s = (coro_socket_t *)client->user_data;
  coro_socket_handle_transport_connect(s, status);
}

/* ── KCP Connect ──────────────────────────────────────────── */

static int kcp_connect(coro_socket_t *s, const char *host, int port) {
  UNUSED(host);
  
  retain_client(s);
  int r = turbo_kcp_client_connect(
      s->handle.kcp, 
      s->resolved_ip, 
      (unsigned short)port,
      on_kcp_connect, 
      on_kcp_recv
  );
  
  if (r != 0) {
    release_client(s);
    return r;
  }
  
  coro_set_wait(s);
  coro_yield();
  return s->status;
}

/* ── KCP Send/Recv ────────────────────────────────────────── */

static int kcp_send(coro_socket_t *s, const char *data, size_t len) {
  return turbo_kcp_client_send(s->handle.kcp, data, len);
}

static int kcp_recv_start(coro_socket_t *s) {
  UNUSED(s);
  return 0; /* KCP recv is always active */
}

static void kcp_recv_stop(coro_socket_t *s) {
  UNUSED(s);
}

/* ── KCP Close ────────────────────────────────────────────── */

static void kcp_close(coro_socket_t *s) {
  if (s->handle.kcp_server) {
    turbo_kcp_server_stop(s->handle.kcp_server);
  } else if (s->handle.kcp) {
    if (s->handle.kcp->connected || s->handle.kcp->connecting) {
      turbo_kcp_client_close(s->handle.kcp);
    }
  }
}

/* ── KCP Transport Ops ────────────────────────────────────── */

const coro_transport_ops_t transport_ops_kcp = {
    .connect = kcp_connect,
    .bind = NULL,
    .listen = NULL,
    .accept = NULL,
    .send = kcp_send,
    .recv_start = kcp_recv_start,
    .recv_stop = kcp_recv_stop,
    .get_local_addr = NULL,
    .close = kcp_close
};
