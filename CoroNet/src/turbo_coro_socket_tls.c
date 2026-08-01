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
                                 turbo_connect_cb on_connect, turbo_close_cb on_close);

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
  retain_client(s);
  if (s) {
    if (s->wait_metric_tls_handshake) {
      s->wait_handler_entry_ns = turbo_hrtime();
    }
    coro_socket_handle_transport_recv(s, slice);
  }
  release_client(s);
  return 0;
}

static void on_tls_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  retain_client(s);
  if (s) {
    if (s->wait_metric_tls_handshake) {
      s->wait_handler_entry_ns = turbo_hrtime();
    }
    coro_socket_handle_transport_connect(s, status);
  }
  release_client(s);
}

static void on_tls_write_complete(turbo_stream_t *stream, int status) {
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  coro_t *co;

  retain_client(s);
  if (!s || !s->co_write_wait) {
    release_client(s);
    return;
  }

  s->write_status = status;
  co = s->co_write_wait;
  s->co_write_wait = NULL;
  coro_resume_co(s->ctx, co);
  release_client(s);
}

static void on_tls_close(void *handle) {
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  stream->managed = 0;
  stream->destroyed = 1;
  stream->on_recv = NULL;
  stream->on_connect = NULL;
  stream->on_write_complete = NULL;
  if (s) {
    int release_accepted_ref = s->accepted_ref;
    retain_client(s);
    s->accepted_ref = 0;
    turbo_stream_set_user_data(stream, NULL);
    coro_socket_handle_transport_close(s);
    if (release_accepted_ref) {
      release_client(s);
    }
    release_client(s);
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
  turbo_tls_client_config_t tls_config;

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
    {
      int rc = coro_socket_apply_stream_options(s);
      if (rc != 0) {
        tls_discard_stream(s);
        return rc;
      }
    }

    if (s->tls_client_configured) {
      memset(&tls_config, 0, sizeof(tls_config));
      tls_config.ca_file = s->tls_ca_file;
      tls_config.cert_file = s->tls_cert_file;
      tls_config.key_file = s->tls_key_file;
      tls_config.key_password = s->tls_key_password;
      tls_config.cipher_list = s->tls_cipher_list;
      tls_config.verify_peer = s->tls_verify_peer;
      if (turbo_stream_tls_set_client_config(s->handle.stream, &tls_config) != 0) {
        tls_discard_stream(s);
        return socket_ctx_error(s, TURBO_EIO);
      }
    }

    /* Important: Set SNI for TLS handshake */
    turbo_stream_tls_set_sni(s->handle.stream, host);

    turbo_stream_set_user_data(s->handle.stream, s);
    turbo_stream_set_write_cb(s->handle.stream, on_tls_write_complete);
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
    if (timed_out) {
      s->timed_out = 0;
    }
    if (status != 0) {
      s->connected = 0;
      tls_discard_stream(s);
    }
    tls_clear_wait_metric(s);
    if (timed_out) {
      release_client(s);
    }
    coro_socket_release_destroy_wait_refs(s);
    return status;
  }
}

int coro_socket_upgrade_tls(coro_socket_t *s, const char *hostname) {
  turbo_stream_t *tcp_stream;
  turbo_stream_t *tls_stream;
  turbo_tls_client_config_t tls_config;
  int rc;

  if (!s || !s->ctx) {
    return TURBO_EINVAL;
  }
  if (s->transport != TURBO_TCP) return TURBO_ENOTSUP;
  if (!s->handle.stream || !s->connected) return TURBO_ENOTCONN;
  if (s->co_wait) {
    return TURBO_EBUSY;
  }

  tcp_stream = s->handle.stream;
  tls_stream = turbo_stream_create(s->ctx, TURBO_STREAM_TLS);
  if (!tls_stream) {
    return socket_ctx_error(s, TURBO_EIO);
  }

  if (s->tls_client_configured) {
    memset(&tls_config, 0, sizeof(tls_config));
    tls_config.ca_file = s->tls_ca_file;
    tls_config.cert_file = s->tls_cert_file;
    tls_config.key_file = s->tls_key_file;
    tls_config.key_password = s->tls_key_password;
    tls_config.cipher_list = s->tls_cipher_list;
    tls_config.verify_peer = s->tls_verify_peer;
    rc = turbo_stream_tls_set_client_config(tls_stream, &tls_config);
    if (rc != 0) {
      TLOG_ERROR("TLS upgrade client configuration failed rc={}", rc);
      turbo_stream_destroy(tls_stream);
      return rc;
    }
  }

  turbo_stream_set_user_data(tls_stream, s);
  turbo_stream_set_write_cb(tls_stream, on_tls_write_complete);
  tls_stream->managed = 1;
  {
    turbo_stream_t *saved = s->handle.stream;
    s->handle.stream = tls_stream;
    rc = coro_socket_apply_stream_options(s);
    s->handle.stream = saved;
    if (rc != 0) {
      TLOG_ERROR("TLS upgrade stream option application failed rc={}", rc);
      turbo_stream_set_user_data(tls_stream, NULL);
      tls_stream->managed = 0;
      turbo_stream_destroy(tls_stream);
      return rc;
    }
  }

  retain_client(s);
  s->status = 0;
  s->connected = 0;
  s->wait_metric_tls_handshake = 1;
  s->wait_metric_stream = tls_stream;
  coro_set_wait(s);
  start_timeout_timer(s);
  {
    uint64_t wrap_start_ns = turbo_hrtime();
    rc = turbo_stream_tls_wrap_client(tls_stream, tcp_stream, hostname, on_tls_connect,
                                      on_tls_close);
    if (rc == 0) {
      turbo_stream_tls_note_wrap_client_time(turbo_hrtime() - wrap_start_ns);
    } else {
      TLOG_ERROR("TLS upgrade attach failed rc={} tcp_kind={} tcp_connected={}", rc,
                 (int)tcp_stream->kind, tcp_stream->connected);
    }
  }
  if (rc != 0) {
    stop_timeout_timer(s);
    s->co_wait = NULL;
    s->connected = 1;
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
  if (s->co_wait) {
    coro_yield();
  }
  {
    tls_note_wait_resume(s);
    int status = s->status;
    int timed_out = s->timed_out;
    if (timed_out) {
      s->timed_out = 0;
    }
    tls_clear_wait_metric(s);
    if (timed_out) {
      release_client(s);
    }
    coro_socket_release_destroy_wait_refs(s);
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
  turbo_stream_set_write_cb(tls_stream, on_tls_write_complete);
  tls_stream->managed = 1;

  retain_client(s);
  coro_set_wait(s);
  rc = turbo_stream_tls_wrap_server_with_context(tls_stream, tcp_stream, s->tls_server_context,
                                                 on_tls_connect, on_tls_close);
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
    if (timed_out) {
      s->timed_out = 0;
    }
    tls_clear_wait_metric(s);
    if (timed_out) {
      release_client(s);
    }
    coro_socket_release_destroy_wait_refs(s);
    return status;
  }
}

int coro_socket_tls_export_channel_binding(const coro_socket_t *s,
                                           uint8_t output[CORO_TLS_CHANNEL_BINDING_SIZE]) {
  if (!output) {
    return TURBO_EINVAL;
  }
  memset(output, 0, CORO_TLS_CHANNEL_BINDING_SIZE);

  if (!s) {
    return TURBO_EINVAL;
  }
  if (s->transport == TURBO_TLS) {
    if (!s->handle.stream) {
      return TURBO_ENOTCONN;
    }
    return turbo_stream_tls_export_channel_binding_internal(s->handle.stream, output,
                                                            CORO_TLS_CHANNEL_BINDING_SIZE);
  }
  if (s->transport == TURBO_WEBSOCKET) {
    if (!s->handle.stream) {
      return TURBO_ENOTCONN;
    }
    if (s->handle.stream->kind == TURBO_STREAM_WSS) {
      return turbo_stream_wss_export_channel_binding_internal(s->handle.stream, output,
                                                              CORO_TLS_CHANNEL_BINDING_SIZE);
    }
  }
  return TURBO_ENOTSUP;
}

int coro_socket_tls_get_verified_peer_certificate_sha256(
    const coro_socket_t *s, char output[CORO_TLS_PEER_CERT_SHA256_CAPACITY]) {
  if (!output) {
    return TURBO_EINVAL;
  }
  memset(output, 0, CORO_TLS_PEER_CERT_SHA256_CAPACITY);

  if (!s) {
    return TURBO_EINVAL;
  }
  if (s->transport == TURBO_TLS) {
    if (!s->handle.stream) {
      return TURBO_ENOTCONN;
    }
    return turbo_stream_tls_get_verified_peer_certificate_sha256_internal(
        s->handle.stream, output, CORO_TLS_PEER_CERT_SHA256_CAPACITY);
  }
  if (s->transport == TURBO_WEBSOCKET) {
    if (!s->handle.stream) {
      return TURBO_ENOTCONN;
    }
    if (s->handle.stream->kind == TURBO_STREAM_WSS) {
      return turbo_stream_wss_get_verified_peer_certificate_sha256_internal(
          s->handle.stream, output, CORO_TLS_PEER_CERT_SHA256_CAPACITY);
    }
  }
  return TURBO_ENOTSUP;
}

/* ── Send/Recv ────────────────────────────────────────────── */

static int tls_begin_write_wait(coro_socket_t *s, coro_t **co_out, int *scheduled_out) {
  coro_t *co;

  if (!s || !co_out || !scheduled_out) return TURBO_EINVAL;
  if (!s->handle.stream || !s->connected) return TURBO_ENOTCONN;

  co = coro_running();
  *co_out = co;
  *scheduled_out = co ? coro_is_scheduled(co) : 0;
  if (!co) return TURBO_OK;
  if (s->co_write_wait) return TURBO_EBUSY;

  s->write_status = 0;
  s->co_write_wait = co;
  if (*scheduled_out) coro_set_waiting_for_io(co, 1);
  return TURBO_OK;
}

static int tls_finish_write_wait(coro_socket_t *s, coro_t *co, int scheduled, int submit_status) {
  if (!co) return submit_status;
  if (submit_status != TURBO_OK) {
    s->co_write_wait = NULL;
    if (scheduled) coro_set_waiting_for_io(co, 0);
    return submit_status;
  }

  while (s->co_write_wait == co)
    coro_yield();
  return s->write_status;
}

static int tls_send(coro_socket_t *s, const char *data, size_t len) {
  coro_t *co;
  int scheduled;
  int rc = tls_begin_write_wait(s, &co, &scheduled);
  if (rc != TURBO_OK) return rc;
  rc = turbo_stream_send(s->handle.stream, data, len);
  return tls_finish_write_wait(s, co, scheduled, rc);
}

static int tls_recv_start(coro_socket_t *s) {
  return turbo_stream_recv_start(s->handle.stream, on_tls_recv);
}

static void tls_recv_stop(coro_socket_t *s) { turbo_stream_recv_stop(s->handle.stream); }

static int tls_get_local_addr(coro_socket_t *s, struct sockaddr_storage *addr) {
  if (s->handle.stream) return turbo_stream_get_local_addr(s->handle.stream, addr);
  return TURBO_ENOTSUP;
}

static int tls_get_peer_addr(coro_socket_t *s, struct sockaddr_storage *addr) {
  if (s && s->handle.stream) return turbo_stream_get_peer_addr(s->handle.stream, addr);
  return TURBO_ENOTSUP;
}

static mem_buffer_t *tls_get_send_buffer(coro_socket_t *s, size_t min_size) {
  return turbo_stream_get_send_buffer(s->handle.stream, min_size);
}

static int tls_send_buffer(coro_socket_t *s, mem_buffer_t *buffer, size_t len) {
  coro_t *co;
  int scheduled;
  int rc = tls_begin_write_wait(s, &co, &scheduled);
  if (rc != TURBO_OK) return rc;
  rc = turbo_stream_send_buffer(s->handle.stream, buffer, len);
  return tls_finish_write_wait(s, co, scheduled, rc);
}

static void tls_close(coro_socket_t *s) {
  turbo_stream_t *stream = s->handle.stream;
  if (!stream || !s->owns_handle) return;
  s->handle.stream = NULL;
  s->close_pending = 1;
  retain_client(s);
  stream->destroyed = 1;
  stream->on_recv = NULL;
  stream->on_connect = NULL;
  stream->on_write_complete = NULL;
  stream->on_close = on_tls_close;
  turbo_stream_close(stream);
}

/* ── Ops table (Mirrors TCP but targeting TLS stream) ──────── */

const coro_transport_ops_t transport_ops_tls = {.connect = tls_connect,
                                                .send = tls_send,
                                                .recv_start = tls_recv_start,
                                                .recv_stop = tls_recv_stop,
                                                .get_local_addr = tls_get_local_addr,
                                                .get_peer_addr = tls_get_peer_addr,
                                                .close = tls_close,
                                                .get_send_buffer = tls_get_send_buffer,
                                                .send_buffer = tls_send_buffer};
