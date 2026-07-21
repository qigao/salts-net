/**
 * @file turbo_coro_socket_ws.c
 * @brief WebSocket transport for coroutine sockets delegates to turbo_stream_t.
 *
 * This is the missing bridge between coro_socket_t and turbo_stream_ws.
 * Without it, coro_socket_connect_ws() was just a raw TCP/TLS connect and never
 * performed the HTTP Upgrade handshake.
 */

#include "CoroNet/turbo_coro_internal.h"
#include "turbo_error.h"
#include "turbo_stream_internal.h"
#include "websocket_frame_parser.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
#endif

extern const coro_transport_ops_t transport_ops_ws;
const coro_transport_ops_t ws_server_ops;
int turbo_stream_ws_wrap_client(turbo_stream_t *ws_stream, turbo_stream_t *tcp_stream,
                                turbo_connect_cb on_connect, turbo_close_cb on_close);
int turbo_stream_ws_wrap_server(turbo_stream_t *ws_stream, turbo_stream_t *tcp_stream,
                                turbo_connect_cb on_connect, turbo_close_cb on_close);

typedef struct ws_connect_state_s {
  char path[256];
  char request_host[256];
  char subprotocol[128];
  int is_tls;
} ws_connect_state_t;

static int socket_ctx_error(coro_socket_t *s, int fallback) {
  return (s && s->ctx && s->ctx->last_error != 0) ? s->ctx->last_error : fallback;
}

static int socket_return_error(coro_socket_t *s, int err) {
  if (s && s->ctx) {
    s->ctx->last_error = err;
  }
  return err;
}

static uint64_t ws_connect_deadline(coro_socket_t *s, uint64_t timeout_ms) {
  uint64_t now;

  if (!s || !s->loop || timeout_ms == 0U) return 0U;
  now = turbo_loop_now(s->loop);
  if (UINT64_MAX - now < timeout_ms) return UINT64_MAX;
  return now + timeout_ms;
}

static int ws_apply_connect_deadline(coro_socket_t *s, uint64_t deadline_ms) {
  uint64_t now;

  if (!s || deadline_ms == 0U) return 0;
  now = turbo_loop_now(s->loop);
  if (now >= deadline_ms) return TURBO_ETIMEDOUT;
  s->timeout_ms = deadline_ms - now;
  return 0;
}

static ws_connect_state_t *ws_state(coro_socket_t *s) {
  return (ws_connect_state_t *)(s ? s->native_tcp_state : NULL);
}

static void ws_configure_socket(coro_socket_t *s) {
  if (!s) {
    return;
  }
  s->transport = TURBO_WEBSOCKET;
  s->ops = &transport_ops_ws;
  s->connected = 0;
  s->status = 0;
}

static void ws_configure_server_socket(coro_socket_t *s) {
  if (!s) {
    return;
  }
  s->transport = TURBO_WEBSOCKET;
  s->ops = &ws_server_ops;
  s->connected = 0;
  s->status = 0;
}

static int ws_store_config(coro_socket_t *s, const char *request_host, const char *path, int is_tls,
                           const char *subprotocol) {
  ws_connect_state_t *cfg;
  const char *actual_path;
  const char *actual_request_host;
  size_t path_len;
  size_t request_host_len;
  size_t subprotocol_len = 0;

  if (!s) {
    return socket_return_error(s, TURBO_EINVAL);
  }

  actual_path = (path && path[0] != '\0') ? path : "/";
  actual_request_host = (request_host && request_host[0] != '\0') ? request_host : "";
  path_len = strlen(actual_path);
  request_host_len = strlen(actual_request_host);
  if (path_len >= sizeof(((ws_connect_state_t *)0)->path)) {
    return socket_return_error(s, TURBO_EINVAL);
  }
  if (request_host_len >= sizeof(((ws_connect_state_t *)0)->request_host)) {
    return socket_return_error(s, TURBO_EINVAL);
  }
  if (subprotocol && subprotocol[0] != '\0') {
    subprotocol_len = strlen(subprotocol);
    if (subprotocol_len >= sizeof(((ws_connect_state_t *)0)->subprotocol)) {
      return socket_return_error(s, TURBO_EINVAL);
    }
  }

  cfg = ws_state(s);
  if (!cfg) {
    cfg = (ws_connect_state_t *)calloc(1, sizeof(*cfg));
    if (!cfg) {
      return socket_return_error(s, TURBO_ENOMEM);
    }
    s->native_tcp_state = cfg;
  }

  memset(cfg, 0, sizeof(*cfg));
  memcpy(cfg->path, actual_path, path_len + 1);
  if (request_host_len > 0) {
    memcpy(cfg->request_host, actual_request_host, request_host_len + 1);
  }
  if (subprotocol_len > 0) {
    memcpy(cfg->subprotocol, subprotocol, subprotocol_len + 1);
  }
  cfg->is_tls = is_tls ? 1 : 0;
  return 0;
}

static int ws_build_sockaddr(const char *ip, int port, struct sockaddr_storage *storage,
                             struct sockaddr **out_addr) {
  struct sockaddr *sa;

  if (!ip || !storage || !out_addr) {
    return TURBO_EINVAL;
  }

  memset(storage, 0, sizeof(*storage));
  sa = (struct sockaddr *)storage;

  if (inet_pton(AF_INET, ip, &((struct sockaddr_in *)sa)->sin_addr) == 1) {
    ((struct sockaddr_in *)sa)->sin_family = AF_INET;
    ((struct sockaddr_in *)sa)->sin_port = htons((unsigned short)port);
    *out_addr = sa;
    return 0;
  }

  if (inet_pton(AF_INET6, ip, &((struct sockaddr_in6 *)sa)->sin6_addr) == 1) {
    ((struct sockaddr_in6 *)sa)->sin6_family = AF_INET6;
    ((struct sockaddr_in6 *)sa)->sin6_port = htons((unsigned short)port);
    *out_addr = sa;
    return 0;
  }

  return TURBO_EINVAL;
}

static turbo_stream_kind_t ws_stream_kind(const ws_connect_state_t *cfg) {
  return (cfg && cfg->is_tls) ? TURBO_STREAM_WSS : TURBO_STREAM_WS;
}

static int on_ws_recv(void *handle, const mem_slice_t *slice, void *peer) {
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  retain_client(s);
  s->recv_ws_opcode = (uint8_t)(uintptr_t)peer;
  coro_socket_handle_transport_recv(s, slice);
  release_client(s);
  return 0;
}

static void on_ws_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  retain_client(s);
  coro_socket_handle_transport_connect(s, status);
  release_client(s);
}

static void on_ws_write_complete(turbo_stream_t *stream, int status) {
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

static void on_ws_close(void *handle) {
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

static int ws_begin_write_wait(coro_socket_t *s, int *scheduled_out) {
  coro_t *co;
  int scheduled;

  if (!s || !scheduled_out) {
    return TURBO_EINVAL;
  }

  co = coro_running();
  if (!co) {
    *scheduled_out = 0;
    return 0;
  }
  if (s->co_write_wait) {
    return TURBO_EBUSY;
  }

  s->write_status = 0;
  s->co_write_wait = co;
  scheduled = coro_is_scheduled(co);
  if (scheduled) {
    coro_set_waiting_for_io(co, 1);
  }
  *scheduled_out = scheduled ? 1 : -1;
  return 0;
}

static int ws_finish_write_wait(coro_socket_t *s, int rc, int scheduled_state) {
  if (!s || scheduled_state == 0) {
    return rc;
  }

  if (rc != 0) {
    s->co_write_wait = NULL;
    if (scheduled_state > 0) {
      coro_set_waiting_for_io(coro_running(), 0);
    }
    return rc;
  }

  if (s->co_write_wait) {
    coro_yield();
  }
  return s->write_status;
}

static void ws_discard_stream(coro_socket_t *s) {
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

static int ws_connect(coro_socket_t *s, const char *host, int port) {
  const char *ip;
  const char *request_host;
  struct sockaddr *sa = NULL;
  turbo_tls_client_config_t tls_config;
  ws_connect_state_t *cfg;

  if (!s || !host) {
    return TURBO_EINVAL;
  }

  cfg = ws_state(s);
  if (!cfg) {
    return TURBO_EINVAL;
  }

  request_host = cfg->request_host[0] ? cfg->request_host : host;

  ip = s->resolved_ip[0] ? s->resolved_ip : host;
  if (ws_build_sockaddr(ip, port, &s->peer_addr, &sa) != 0) {
    return TURBO_EINVAL;
  }

  if (s->handle.stream && !s->connected) {
    ws_discard_stream(s);
  }

  if (!s->handle.stream) {
    s->handle.stream = turbo_stream_create(s->ctx, ws_stream_kind(cfg));
    if (!s->handle.stream) {
      return socket_ctx_error(s, TURBO_EIO);
    }
    {
      int rc = coro_socket_apply_stream_options(s);
      if (rc != 0) {
        ws_discard_stream(s);
        return rc;
      }
    }

    if (cfg->is_tls && s->tls_client_configured) {
      memset(&tls_config, 0, sizeof(tls_config));
      tls_config.ca_file = s->tls_ca_file;
      tls_config.cert_file = s->tls_cert_file;
      tls_config.key_file = s->tls_key_file;
      tls_config.key_password = s->tls_key_password;
      tls_config.cipher_list = s->tls_cipher_list;
      tls_config.verify_peer = s->tls_verify_peer;
      if (turbo_stream_tls_set_client_config(s->handle.stream, &tls_config) != 0) {
        ws_discard_stream(s);
        return socket_ctx_error(s, TURBO_EIO);
      }
    }

    turbo_stream_ws_set_path_host_protocol(s->handle.stream, cfg->path[0] ? cfg->path : "/", request_host,
                                           cfg->subprotocol[0] ? cfg->subprotocol : NULL);
    turbo_stream_set_user_data(s->handle.stream, s);
    turbo_stream_set_write_cb(s->handle.stream, on_ws_write_complete);
    s->handle.stream->managed = 1;
  }

  retain_client(s);
  coro_set_wait(s);
  {
    int rc = turbo_stream_connect_addr(s->handle.stream, sa, on_ws_connect, on_ws_close);
    if (rc != 0) {
      s->co_wait = NULL;
      release_client(s);
      ws_discard_stream(s);
      return rc;
    }
  }

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
    if (status != 0) {
      s->connected = 0;
      ws_discard_stream(s);
    }
    return status;
  }
}

static int ws_send(coro_socket_t *s, const char *data, size_t len) {
  int scheduled_state = 0;
  int rc = ws_begin_write_wait(s, &scheduled_state);
  if (rc != 0) {
    return rc;
  }
  rc = turbo_stream_send(s->handle.stream, data, len);
  return ws_finish_write_wait(s, rc, scheduled_state);
}

int coro_socket_send_ws_text(coro_socket_t *s, const char *text, size_t len) {
  int scheduled_state = 0;
  int rc;

  if (!s || !text || len == 0) {
    return socket_return_error(s, TURBO_EINVAL);
  }
  if (s->transport != TURBO_WEBSOCKET || !s->handle.stream) {
    return socket_return_error(s, TURBO_ENOTSUP);
  }

  rc = ws_begin_write_wait(s, &scheduled_state);
  if (rc != 0) {
    return socket_return_error(s, rc);
  }
  rc = turbo_stream_ws_send_text(s->handle.stream, text, len);
  return socket_return_error(s, ws_finish_write_wait(s, rc, scheduled_state));
}

int coro_socket_recv_ws(coro_socket_t *s, char **data, size_t *len, int *is_text) {
  int rc;

  if (!s || s->transport != TURBO_WEBSOCKET) {
    return socket_return_error(s, TURBO_ENOTSUP);
  }

  rc = coro_socket_recv(s, data, len);
  if (rc == 0 && is_text) {
    *is_text = (s->recv_ws_opcode == WS_OPCODE_TEXT);
  }
  return socket_return_error(s, rc);
}

static int ws_send_owned_recv(coro_socket_t *s, char *data, size_t len) {
  int scheduled_state = 0;
  int rc = ws_begin_write_wait(s, &scheduled_state);
  if (rc != 0) {
    return rc;
  }
  rc = turbo_stream_ws_send_owned_recv(s->handle.stream, data, len);
  return ws_finish_write_wait(s, rc, scheduled_state);
}

static int ws_recv_start(coro_socket_t *s) {
  return turbo_stream_recv_start(s->handle.stream, on_ws_recv);
}

static void ws_recv_stop(coro_socket_t *s) { turbo_stream_recv_stop(s->handle.stream); }

static int ws_get_local_addr(coro_socket_t *s, struct sockaddr_storage *addr) {
  if (s->handle.stream) {
    return turbo_stream_get_local_addr(s->handle.stream, addr);
  }
  return TURBO_ENOTSUP;
}

static mem_buffer_t *ws_get_send_buffer(coro_socket_t *s, size_t min_size) {
  return turbo_stream_get_send_buffer(s->handle.stream, min_size);
}

static int ws_send_buffer(coro_socket_t *s, mem_buffer_t *buffer, size_t len) {
  int scheduled_state = 0;
  int rc = ws_begin_write_wait(s, &scheduled_state);
  if (rc != 0) {
    return rc;
  }
  rc = turbo_stream_send_buffer(s->handle.stream, buffer, len);
  return ws_finish_write_wait(s, rc, scheduled_state);
}

static void ws_close(coro_socket_t *s) {
  turbo_stream_t *stream = s->handle.stream;
  if (s->native_tcp_state) {
    free(s->native_tcp_state);
    s->native_tcp_state = NULL;
  }
  if (!stream || !s->owns_handle) {
    return;
  }
  s->handle.stream = NULL;
  s->close_pending = 1;
  retain_client(s);
  stream->on_recv = NULL;
  stream->on_connect = NULL;
  stream->on_write_complete = NULL;
  stream->on_close = on_ws_close;
  turbo_stream_close(stream);
}

const coro_transport_ops_t transport_ops_ws = {.connect = ws_connect,
                                               .send = ws_send,
                                               .send_owned_recv = ws_send_owned_recv,
                                               .recv_start = ws_recv_start,
                                               .recv_stop = ws_recv_stop,
                                               .get_local_addr = ws_get_local_addr,
                                               .close = ws_close,
                                               .get_send_buffer = ws_get_send_buffer,
                                               .send_buffer = ws_send_buffer};

const coro_transport_ops_t ws_server_ops = {.send = ws_send,
                                            .send_owned_recv = ws_send_owned_recv,
                                            .recv_start = ws_recv_start,
                                            .recv_stop = ws_recv_stop,
                                            .get_local_addr = ws_get_local_addr,
                                            .close = ws_close,
                                            .get_send_buffer = ws_get_send_buffer,
                                            .send_buffer = ws_send_buffer};

int coro_socket_connect_ws(coro_socket_t *s, const char *host, int port, const char *path,
                           int is_tls) {
  return coro_socket_connect_ws_ex(s, host, port, path, is_tls, NULL);
}

int coro_socket_connect_ws_ex(coro_socket_t *s, const char *host, int port, const char *path,
                              int is_tls, const char *subprotocol) {
  return coro_socket_connect_ws_host_ex(s, host, port, host, path, is_tls, subprotocol);
}

int coro_socket_connect_ws_host_ex(coro_socket_t *s, const char *connect_host, int port,
                                   const char *request_host, const char *path, int is_tls,
                                   const char *subprotocol) {
  const char *actual_request_host;
  if (!s || !connect_host) {
    return socket_return_error(s, TURBO_EINVAL);
  }

  if (s->connected) {
    return socket_return_error(s, TURBO_EALREADY);
  }

  actual_request_host =
      (request_host && request_host[0] != '\0') ? request_host : connect_host;
  {
    int rc = ws_store_config(s,
                             actual_request_host, path, is_tls, subprotocol);
    if (rc != 0) {
      return rc;
    }
  }

  if (s->proxy.type != CORO_PROXY_DIRECT) {
    uint64_t saved_timeout = s->timeout_ms;
    uint64_t deadline_ms = ws_connect_deadline(s, saved_timeout);
    int rc;

    coro_socket_configure_transport_internal(s, TURBO_TCP, 0);
    rc = coro_socket_connect_host_ex(s, connect_host, port, actual_request_host);
    if (rc == 0 && is_tls) {
      rc = ws_apply_connect_deadline(s, deadline_ms);
      if (rc == 0) rc = coro_socket_upgrade_tls(s, actual_request_host);
    }
    if (rc == 0) {
      rc = ws_apply_connect_deadline(s, deadline_ms);
      if (rc == 0) rc = coro_socket_upgrade_ws_ex(s, actual_request_host, path, subprotocol);
    }
    s->timeout_ms = saved_timeout;
    return rc;
  }

  ws_configure_socket(s);
  return coro_socket_connect(s, connect_host, port);
}

int coro_socket_upgrade_ws_ex(coro_socket_t *s, const char *request_host,
                              const char *path, const char *subprotocol) {
  turbo_stream_kind_t kind;
  turbo_stream_t *raw_stream;
  turbo_stream_t *ws_stream;
  const char *actual_request_host;
  int is_tls;
  int rc;

  if (!s || !s->ctx) {
    return TURBO_EINVAL;
  }
  if ((s->transport != TURBO_TCP && s->transport != TURBO_TLS) || !s->handle.stream || !s->connected) {
    return socket_return_error(s, TURBO_EINVAL);
  }
  if (s->co_wait) {
    return socket_return_error(s, TURBO_EBUSY);
  }

  actual_request_host = request_host;
  if ((!actual_request_host || actual_request_host[0] == '\0') &&
      s->resolved_ip[0] != '\0') {
    actual_request_host = s->resolved_ip;
  }

  is_tls = (s->transport == TURBO_TLS);
  rc = ws_store_config(s, actual_request_host, path, is_tls, subprotocol);
  if (rc != 0) {
    return rc;
  }

  kind = is_tls ? TURBO_STREAM_WSS : TURBO_STREAM_WS;
  raw_stream = s->handle.stream;
  ws_stream = turbo_stream_create(s->ctx, kind);
  if (!ws_stream) {
    return socket_ctx_error(s, TURBO_EIO);
  }

  turbo_stream_ws_set_path_host_protocol(
      ws_stream, path && path[0] != '\0' ? path : "/",
      (actual_request_host && actual_request_host[0] != '\0') ? actual_request_host : NULL,
      (subprotocol && subprotocol[0] != '\0') ? subprotocol : NULL);
  turbo_stream_set_user_data(ws_stream, s);
  turbo_stream_set_write_cb(ws_stream, on_ws_write_complete);
  ws_stream->managed = 1;

  s->handle.stream = ws_stream;
  ws_configure_socket(s);
  s->owns_handle = 1;

  retain_client(s);
  coro_set_wait(s);
  start_timeout_timer(s);
  rc = turbo_stream_ws_wrap_client(ws_stream, raw_stream, on_ws_connect, on_ws_close);
  if (rc != 0) {
    stop_timeout_timer(s);
    s->co_wait = NULL;
    turbo_stream_set_user_data(ws_stream, NULL);
    ws_stream->managed = 0;
    s->handle.stream = NULL;
    s->connected = 0;
    turbo_stream_destroy(ws_stream);
    release_client(s);
    return rc;
  }

  if (s->co_wait) {
    coro_yield();
  }
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
    return status;
  }
}

int coro_socket_wrap_accepted_ws_server(coro_socket_t *s) {
  turbo_stream_kind_t kind;
  turbo_stream_t *raw_stream;
  turbo_stream_t *ws_stream;
  int rc;

  if (!s || !s->handle.stream || !s->connected) {
    return TURBO_EINVAL;
  }

  kind = (s->transport == TURBO_TLS) ? TURBO_STREAM_WSS : TURBO_STREAM_WS;
  raw_stream = s->handle.stream;
  ws_stream = turbo_stream_create(s->ctx, kind);
  if (!ws_stream) {
    return socket_ctx_error(s, TURBO_EIO);
  }

  s->handle.stream = ws_stream;
  ws_configure_server_socket(s);
  s->owns_handle = 1;
  turbo_stream_set_user_data(ws_stream, s);
  turbo_stream_set_write_cb(ws_stream, on_ws_write_complete);
  ws_stream->managed = 1;
  if (s->ws_server_configured) {
    coro_ws_server_config_t config = CORO_WS_SERVER_CONFIG_DEFAULT;
    config.path = s->ws_server_path[0] ? s->ws_server_path : NULL;
    config.subprotocol = s->ws_server_subprotocol[0]
                             ? s->ws_server_subprotocol
                             : NULL;
    config.max_message_size = s->ws_server_max_message_size;
    config.binary_only = s->ws_server_binary_only;
    rc = turbo_stream_ws_set_server_config_internal(
        ws_stream, config.path, config.subprotocol, config.max_message_size,
        config.binary_only);
    if (rc != 0) {
      turbo_stream_set_user_data(ws_stream, NULL);
      s->handle.stream = NULL;
      s->connected = 0;
      turbo_stream_destroy(ws_stream);
      return rc;
    }
  }
  {
    rc = coro_socket_apply_stream_options(s);
    if (rc != 0) {
      turbo_stream_set_user_data(ws_stream, NULL);
      s->handle.stream = NULL;
      s->connected = 0;
      turbo_stream_destroy(ws_stream);
      return rc;
    }
  }

  rc = turbo_stream_recv_start(ws_stream, on_ws_recv);
  if (rc != 0 && rc != TURBO_EALREADY) {
    turbo_stream_set_user_data(ws_stream, NULL);
    s->handle.stream = NULL;
    s->connected = 0;
    turbo_stream_destroy(ws_stream);
    return rc;
  }

  retain_client(s);
  coro_set_wait(s);
  rc = turbo_stream_ws_wrap_server(ws_stream, raw_stream, on_ws_connect, on_ws_close);
  if (rc != 0) {
    s->co_wait = NULL;
    release_client(s);
    turbo_stream_set_user_data(ws_stream, NULL);
    s->handle.stream = NULL;
    s->connected = 0;
    turbo_stream_destroy(ws_stream);
    return rc;
  }

  if (s->co_wait) {
    start_timeout_timer(s);
    coro_yield();
  }
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
    return status;
  }
}
