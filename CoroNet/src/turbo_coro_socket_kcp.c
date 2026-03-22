/**
 * @file turbo_coro_socket_kcp.c
 * @brief KCP (reliable UDP) transport implementation.
 */

#include "CoroNet/turbo_coro_internal.h"
#include "CoroNet/turbo_kcp.h"
#include <stdlib.h>
#include <string.h>
#include "turbo_error.h"

/* ── KCP Callbacks ────────────────────────────────────────── */

static int on_kcp_recv(void *handle, const mem_slice_t *slice, void *peer) {
  UNUSED(peer);
  turbo_kcp_t *client = (turbo_kcp_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_kcp_get_user_data(client);
  if (s) {
    coro_socket_handle_transport_recv(s, slice);
  }
  return 0;
}

static void on_kcp_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_kcp_t *client = (turbo_kcp_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_kcp_get_user_data(client);
  coro_socket_handle_transport_connect(s, status);
}

/* ── KCP Connect ──────────────────────────────────────────── */

static int kcp_connect(coro_socket_t *s, const char *host, int port) {
  /* Create handle if not existing */
  if (!s->handle.kcp) {
    s->handle.kcp = turbo_kcp_create(s->ctx);
    if (!s->handle.kcp) return TURBO_ENOMEM;
    turbo_kcp_set_user_data(s->handle.kcp, s);
    s->owns_handle = 1;
  }
  
  retain_client(s);
  int r = turbo_kcp_connect(
      s->handle.kcp, 
      host, 
      port,
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
  return turbo_kcp_send(s->handle.kcp, data, len);
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
  if (s->handle.kcp) {
    turbo_kcp_close(s->handle.kcp);
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
