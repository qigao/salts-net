/**
 * @file turbo_coro_socket_tls.c
 * @brief TLS transport implementation for coroutine sockets.
 */

#include "CoroNet/turbo_coro_internal.h"
#include <stdlib.h>
#include <string.h>

/* ── Forward declarations ─────────────────────────────────── */
static void on_tls_coro_handshake(turbo_tls_client_t *client, int status);

/* ── TLS Callbacks ────────────────────────────────────────── */

static int on_tls_coro_recv(void *handle, const mem_slice_t *slice, void *peer) {
  UNUSED(peer);
  turbo_tls_client_t *tls = (turbo_tls_client_t *)handle;
  coro_socket_t *s = (coro_socket_t *)tls->user_data;
  if (!s) return 0;
  coro_socket_handle_transport_recv(s, slice);
  return 0;
}

static void on_tls_coro_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_tls_client_t *tls = (turbo_tls_client_t *)handle;
  coro_socket_t *s = (coro_socket_t *)tls->user_data;
  if (!s) return;
  
  if (status != 0) {
    coro_socket_handle_transport_connect(s, status);
  }
  
  /* TCP connected, wait for TLS handshake which will fire on_tls_coro_handshake */
}

static void on_tls_coro_handshake(turbo_tls_client_t *client, int status) {
  coro_socket_t *s = (coro_socket_t *)client->user_data;
  if (!s) return;
  
  /* Handshake done, now we can resume the connect coro */
  coro_socket_handle_transport_connect(s, status);
}

static void on_tls_coro_close(void *handle) {
  turbo_tls_client_t *tls = (turbo_tls_client_t *)handle;
  coro_socket_t *s = (coro_socket_t *)tls->user_data;
  if (s) {
    tls->user_data = NULL;
    coro_socket_handle_transport_close(s);
  }
}

/* ── TLS Connect ──────────────────────────────────────────── */

static int tls_connect(coro_socket_t *s, const char *host, int port) {
  if (!s->tls) {
    turbo_tls_context_init(&s->tls_ctx, TURBO_TLS_CONTEXT_LIB_INIT);
    /* In tests we usually don't verify certs for httpbin */
    turbo_tls_context_set_verify_flags(&s->tls_ctx, TURBO_TLS_VERIFY_NONE);
    
    s->tls = turbo_tls_client_create(s->loop, &s->tls_ctx);
    if (!s->tls) return UV_ENOMEM;
    s->tls->user_data = s;
  }
  
  /* Crucial: Set hostname for SNI */
  turbo_tls_client_set_hostname(s->tls, host, strlen(host));
  
  /* Set handshake callback to wake up the coroutine when TLS is really ready */
  s->tls->handshake_done_cb = on_tls_coro_handshake;
  
  retain_client(s);
  int r = turbo_tls_client_connect(
      s->tls, 
      s->resolved_ip, 
      (unsigned short)port,
      on_tls_coro_recv, 
      on_tls_coro_connect, 
      on_tls_coro_close
  );
  
  if (r != 0) {
    release_client(s);
    return r;
  }
  
  coro_set_wait(s);
  coro_yield();
  return s->status;
}

/* ── TLS Send/Recv ────────────────────────────────────────── */

static int tls_send(coro_socket_t *s, const char *data, size_t len) {
  if (s->status != 0) return s->status;
  if (!s->tls) return UV_ENOTCONN;
  
  /* Standard fallback send */
  return turbo_tls_send(s->tls, data, len);
}

static mem_buffer_t *tls_get_send_buffer(coro_socket_t *s, size_t min_size) {
  if (!s->tls) return NULL;
  return turbo_tls_get_send_buffer(s->tls, min_size);
}

static int tls_send_buffer(coro_socket_t *s, mem_buffer_t *buffer, size_t len) {
  if (!s->tls) return UV_ENOTCONN;
  return turbo_tls_send_buffer(s->tls, buffer, len);
}

static int tls_recv_start(coro_socket_t *s) {
  if (!s->tls) return UV_ENOTCONN;
  return turbo_tls_read_start(s->tls, NULL, on_tls_coro_recv);
}

static void tls_recv_stop(coro_socket_t *s) {
  if (s->tls) turbo_tls_read_stop(s->tls);
}

/* ── TLS Close ────────────────────────────────────────────── */

static void tls_close(coro_socket_t *s) {
  turbo_tls_client_t *tls = s->tls;
  if (!tls || !s->owns_handle) return;
  
  s->tls = NULL;
  
  if (!tls->closing) {
    retain_client(s);
    tls->on_close = on_tls_coro_close;
    turbo_tls_client_close(tls);
  }
}

/* ── TLS Transport Ops ────────────────────────────────────── */

const coro_transport_ops_t transport_ops_tls = {
    .connect = tls_connect,
    .bind = NULL,
    .listen = NULL,
    .accept = NULL,
    .send = tls_send,
    .recv_start = tls_recv_start,
    .recv_stop = tls_recv_stop,
    .get_local_addr = NULL,
    .close = tls_close,
    .get_send_buffer = tls_get_send_buffer,
    .send_buffer = tls_send_buffer
};
