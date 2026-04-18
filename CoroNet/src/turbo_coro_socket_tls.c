/**
 * @file turbo_coro_socket_tls.c
 * @brief TLS transport for coroutine sockets — delegates to turbo_stream_t with TLS backend.
 */

#include "CoroNet/turbo_coro_internal.h"
#include "tlog.h"
#include "turbo_error.h"
#include "turbo_stream_internal.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
#endif

extern const coro_transport_ops_t transport_ops_tls;

static int socket_ctx_error(coro_socket_t *s, int fallback) {
  return (s && s->ctx && s->ctx->last_error != 0) ? s->ctx->last_error : fallback;
}

/* Pre-declare SNI setter from turbo_stream_tls.c */
void turbo_stream_tls_set_sni(turbo_stream_t *s, const char *hostname);
void turbo_stream_tls_note_resume_wait(turbo_stream_t *s, uint64_t value_ns);
void turbo_stream_tls_note_wrap_client_time(uint64_t value_ns);
void turbo_stream_tls_note_waiter_signal(turbo_stream_t *s, uint64_t value_ns);
int turbo_stream_tls_wrap_server(turbo_stream_t *tls_stream, turbo_stream_t *tcp_stream,
                                 turbo_connect_cb on_connect,
                                 turbo_close_cb on_close);

static void tls_note_wait_resume(coro_socket_t *s) {
  uint64_t resume_wait_ns;

  if (!s || !s->wait_metric_tls_handshake || !s->wait_metric_stream ||
      s->wait_resume_signal_ns == 0) {
    return;
  }

  resume_wait_ns = turbo_hrtime() - s->wait_resume_signal_ns;
  s->wait_resume_signal_ns = 0;
  turbo_stream_tls_note_resume_wait(s->wait_metric_stream, resume_wait_ns);
  s->wait_metric_tls_handshake = 0;
  s->wait_metric_stream = NULL;
  s->wait_handler_entry_ns = 0;
}

static void tls_clear_wait_metric(coro_socket_t *s) {
  if (!s) {
    return;
  }
  s->wait_metric_tls_handshake = 0;
  s->wait_metric_stream = NULL;
  s->wait_handler_entry_ns = 0;
  s->wait_resume_signal_ns = 0;
}

/* ── Client callbacks (Same as TCP as they use turbo_stream_t) ── */

static int on_tls_recv(void *handle, const mem_slice_t *slice, void *peer) {
  UNUSED(peer);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  if (s) {
    if (s->wait_metric_tls_handshake) {
      s->wait_handler_entry_ns = turbo_hrtime();
    }
    coro_socket_handle_transport_recv(s, slice);
  }
  return 0;
}

static void on_tls_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  if (s) {
    if (s->wait_metric_tls_handshake) {
      s->wait_handler_entry_ns = turbo_hrtime();
    }
    coro_socket_handle_transport_connect(s, status);
  }
}

static void on_tls_close(void *handle) {
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  stream->managed = 0;
  stream->destroyed = 1;
  if (s) {
    int release_accepted_ref = s->accepted_ref;
    s->accepted_ref = 0;
    turbo_stream_set_user_data(stream, NULL);
    coro_socket_handle_transport_close(s);
    if (release_accepted_ref) {
      release_client(s);
    }
  }
}

static void tls_discard_stream(coro_socket_t *s) {
  turbo_stream_t *stream;

  if (!s || !s->handle.stream) {
    return;
  }

  stream = s->handle.stream;
  s->handle.stream = NULL;
  turbo_stream_set_user_data(stream, NULL);
  stream->managed = 0;
  turbo_stream_destroy(stream);
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

  if (s->handle.stream && !s->connected) {
    tls_discard_stream(s);
  }

  if (!s->handle.stream) {
    s->handle.stream = turbo_stream_create(s->ctx, TURBO_STREAM_TLS);
    if (!s->handle.stream) return socket_ctx_error(s, TURBO_EIO);

    /* Important: Set SNI for TLS handshake */
    turbo_stream_tls_set_sni(s->handle.stream, host);

    turbo_stream_set_user_data(s->handle.stream, s);
    s->handle.stream->managed = 1;
  }

  retain_client(s);
  s->wait_metric_tls_handshake = 1;
  s->wait_metric_stream = s->handle.stream;
  coro_set_wait(s);
  int r = turbo_stream_connect_addr(s->handle.stream, sa, on_tls_connect, on_tls_close);
  if (r != 0) {
    s->co_wait = NULL;
    s->wait_metric_tls_handshake = 0;
    s->wait_metric_stream = NULL;
    release_client(s);
    tls_discard_stream(s);
    return r;
  }

  start_timeout_timer(s);
  coro_yield();
  {
    tls_note_wait_resume(s);
    int status = s->status;
    int timed_out = s->timed_out;
    if (s->destroy_wait_handoff) {
      s->destroy_wait_handoff = 0;
      release_client(s);
    }
    if (timed_out) {
      s->timed_out = 0;
      release_client(s);
    }
    if (status != 0) {
      s->connected = 0;
      tls_discard_stream(s);
    }
    tls_clear_wait_metric(s);
    return status;
  }
}

int coro_socket_upgrade_tls(coro_socket_t *s, const char *hostname) {
  turbo_stream_t *tcp_stream;
  turbo_stream_t *tls_stream;
  int rc;

  if (!s || !s->ctx) {
    return TURBO_EINVAL;
  }
  if (s->transport != TURBO_TCP || !s->handle.stream || !s->connected) {
    return TURBO_EINVAL;
  }
  if (s->co_wait) {
    return TURBO_EBUSY;
  }

  tcp_stream = s->handle.stream;
  tls_stream = turbo_stream_create(s->ctx, TURBO_STREAM_TLS);
  if (!tls_stream) {
    return socket_ctx_error(s, TURBO_EIO);
  }

  turbo_stream_set_user_data(tls_stream, s);
  tls_stream->managed = 1;

  retain_client(s);
  {
    uint64_t wrap_start_ns = turbo_hrtime();
  rc = turbo_stream_tls_wrap_client(tls_stream, tcp_stream, hostname, on_tls_connect, on_tls_close);
    if (rc == 0) {
      turbo_stream_tls_note_wrap_client_time(turbo_hrtime() - wrap_start_ns);
    }
  }
  if (rc != 0) {
    turbo_stream_set_user_data(tls_stream, NULL);
    tls_stream->managed = 0;
    turbo_stream_destroy(tls_stream);
    s->wait_metric_tls_handshake = 0;
    s->wait_metric_stream = NULL;
    release_client(s);
    return rc;
  }

  s->handle.stream = tls_stream;
  s->transport = TURBO_TLS;
  s->ops = &transport_ops_tls;
  s->connected = 0;

  s->wait_metric_tls_handshake = 1;
  s->wait_metric_stream = tls_stream;
  coro_set_wait(s);
  start_timeout_timer(s);
  coro_yield();
  {
    tls_note_wait_resume(s);
    int status = s->status;
    int timed_out = s->timed_out;
    if (s->destroy_wait_handoff) {
      s->destroy_wait_handoff = 0;
      release_client(s);
    }
    if (timed_out) {
      s->timed_out = 0;
      release_client(s);
    }
    tls_clear_wait_metric(s);
    return status;
  }
}

int coro_socket_wrap_accepted_tls_server(coro_socket_t *s) {
  turbo_stream_t *tcp_stream;
  turbo_stream_t *tls_stream;
  int rc;

  if (!s || !s->ctx || s->transport != TURBO_TCP || !s->handle.stream || !s->connected) {
    return TURBO_EINVAL;
  }

  tcp_stream = s->handle.stream;
  tls_stream = turbo_stream_create(s->ctx, TURBO_STREAM_TLS);
  if (!tls_stream) {
    return socket_ctx_error(s, TURBO_EIO);
  }

  turbo_stream_set_user_data(tls_stream, s);
  tls_stream->managed = 1;

  retain_client(s);
  coro_set_wait(s);
  rc = turbo_stream_tls_wrap_server(tls_stream, tcp_stream, on_tls_connect, on_tls_close);
  if (rc != 0) {
    s->co_wait = NULL;
    release_client(s);
    TLOG_ERROR("tls server wrap failed rc={}", rc);
    turbo_stream_set_user_data(tls_stream, NULL);
    tls_stream->managed = 0;
    turbo_stream_destroy(tls_stream);
    return rc;
  }

  s->handle.stream = tls_stream;
  s->transport = TURBO_TLS;
  s->ops = &transport_ops_tls;
  s->connected = 0;

  start_timeout_timer(s);
  coro_yield();
  {
    int status = s->status;
    int timed_out = s->timed_out;
    if (s->destroy_wait_handoff) {
      s->destroy_wait_handoff = 0;
      release_client(s);
    }
    if (timed_out) {
      s->timed_out = 0;
      release_client(s);
    }
    tls_clear_wait_metric(s);
    return status;
  }
}

/* ── Send/Recv ────────────────────────────────────────────── */

static int tls_send(coro_socket_t *s, const char *data, size_t len) {
  return turbo_stream_send(s->handle.stream, data, len);
}

static int tls_recv_start(coro_socket_t *s) {
  return turbo_stream_recv_start(s->handle.stream, on_tls_recv);
}

static void tls_recv_stop(coro_socket_t *s) { turbo_stream_recv_stop(s->handle.stream); }

static int tls_get_local_addr(coro_socket_t *s, struct sockaddr_storage *addr) {
  if (s->handle.stream) return turbo_stream_get_local_addr(s->handle.stream, addr);
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
  s->close_pending = 1;
  retain_client(s);
  stream->on_close = on_tls_close;
  turbo_stream_close(stream);
}

/* ── Ops table (Mirrors TCP but targeting TLS stream) ──────── */

const coro_transport_ops_t transport_ops_tls = {.connect = tls_connect,
                                                .send = tls_send,
                                                .recv_start = tls_recv_start,
                                                .recv_stop = tls_recv_stop,
                                                .get_local_addr = tls_get_local_addr,
                                                .close = tls_close,
                                                .get_send_buffer = tls_get_send_buffer,
                                                .send_buffer = tls_send_buffer};
