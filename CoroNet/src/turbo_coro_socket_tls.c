/**
 * @file turbo_coro_socket_tls.c
 * @brief TLS transport for coroutine sockets — delegates to turbo_stream_t with TLS backend.
 */

#include "CoroNet/turbo_coro_internal.h"
#include "turbo_stream_internal.h"
#include "turbo_error.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#endif

extern const coro_transport_ops_t transport_ops_tls;

/* Pre-declare SNI setter from turbo_stream_tls.c */
void turbo_stream_tls_set_sni(turbo_stream_t *s, const char *hostname);

/* ── Client callbacks (Same as TCP as they use turbo_stream_t) ── */

static int on_tls_recv(void *handle, const mem_slice_t *slice, void *peer) {
  UNUSED(peer);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  if (s) {
    coro_socket_handle_transport_recv(s, slice);
  }
  return 0;
}

static void on_tls_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  if (s) {
    coro_socket_handle_transport_connect(s, status);
  }
}

static void on_tls_close(void *handle) {
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  if (s) {
    turbo_stream_set_user_data(stream, NULL);
    coro_socket_handle_transport_close(s);
  }
}

/* ── Connect ──────────────────────────────────────────────── */

static int tls_connect(coro_socket_t *s, const char *host, int port) {
  const char *ip = s->resolved_ip[0] ? s->resolved_ip : host;

  struct sockaddr *sa = (struct sockaddr *)&s->peer_addr;
  memset(sa, 0, sizeof(s->peer_addr));

  if (inet_pton(AF_INET, ip, &((struct sockaddr_in *)sa)->sin_addr) == 1) {
    ((struct sockaddr_in *)sa)->sin_family = AF_INET;
    ((struct sockaddr_in *)sa)->sin_port = htons((unsigned short)port);
  } else if (inet_pton(AF_INET6, ip, &((struct sockaddr_in6 *)sa)->sin6_addr) == 1) {
    ((struct sockaddr_in6 *)sa)->sin6_family = AF_INET6;
    ((struct sockaddr_in6 *)sa)->sin6_port = htons((unsigned short)port);
  } else {
    return TURBO_EINVAL;
  }

  if (!s->handle.stream) {
    s->handle.stream = turbo_stream_create(s->ctx, TURBO_STREAM_TLS);
    if (!s->handle.stream) return TURBO_ENOMEM;
    
    /* Important: Set SNI for TLS handshake */
    turbo_stream_tls_set_sni(s->handle.stream, host);
    
    turbo_stream_set_user_data(s->handle.stream, s);
    s->handle.stream->managed = 1;
  }

  retain_client(s);
  int r = turbo_stream_connect_addr(s->handle.stream, sa,
                                    on_tls_connect, on_tls_close);
  if (r != 0) {
    release_client(s);
    return r;
  }

  coro_set_wait(s);
  coro_yield();
  return s->status;
}

/* ── Send/Recv ────────────────────────────────────────────── */

static int tls_send(coro_socket_t *s, const char *data, size_t len) {
    return turbo_stream_send(s->handle.stream, data, len);
}

static int tls_recv_start(coro_socket_t *s) {
    return turbo_stream_recv_start(s->handle.stream, on_tls_recv);
}

static void tls_recv_stop(coro_socket_t *s) {
    turbo_stream_recv_stop(s->handle.stream);
}

static int tls_get_local_addr(coro_socket_t *s, struct sockaddr_storage *addr) {
  if (s->handle.stream)
    return turbo_stream_get_local_addr(s->handle.stream, addr);
  return TURBO_ENOTSUP;
}

static mem_buffer_t *tls_get_send_buffer(coro_socket_t *s, size_t min_size) {
  return turbo_stream_get_send_buffer(s->handle.stream, min_size);
}

static int tls_send_buffer(coro_socket_t *s, mem_buffer_t *buffer, size_t len) {
  return turbo_stream_send_buffer(s->handle.stream, buffer, len);
}

static void tls_close(coro_socket_t *s) {
  turbo_stream_t *stream = s->handle.stream;
  if (!stream || !s->owns_handle) return;
  s->handle.stream = NULL;
  retain_client(s);
  stream->on_close = on_tls_close;
  turbo_stream_close(stream);
}

/* ── Ops table (Mirrors TCP but targeting TLS stream) ──────── */

const coro_transport_ops_t transport_ops_tls = {
    .connect = tls_connect,
    .send = tls_send,
    .recv_start = tls_recv_start,
    .recv_stop = tls_recv_stop,
    .get_local_addr = tls_get_local_addr,
    .close = tls_close,
    .get_send_buffer = tls_get_send_buffer,
    .send_buffer = tls_send_buffer
};
