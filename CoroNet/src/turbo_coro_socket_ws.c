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
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
#endif

extern const coro_transport_ops_t transport_ops_ws;

typedef struct ws_connect_state_s {
  char path[256];
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
  UNUSED(peer);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  if (s) {
    coro_socket_handle_transport_recv(s, slice);
  }
  return 0;
}

static void on_ws_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  if (s) {
    coro_socket_handle_transport_connect(s, status);
  }
}

static void on_ws_close(void *handle) {
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  if (s) {
    turbo_stream_set_user_data(stream, NULL);
    coro_socket_handle_transport_close(s);
  }
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
  struct sockaddr *sa = NULL;
  ws_connect_state_t *cfg;

  if (!s || !host) {
    return TURBO_EINVAL;
  }

  cfg = ws_state(s);
  if (!cfg) {
    return TURBO_EINVAL;
  }

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

    turbo_stream_ws_set_path_host_protocol(s->handle.stream, cfg->path[0] ? cfg->path : "/", host,
                                           cfg->subprotocol[0] ? cfg->subprotocol : NULL);
    turbo_stream_set_user_data(s->handle.stream, s);
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

  coro_yield();
  {
    int status = s->status;
    if (s->destroy_wait_handoff) {
      s->destroy_wait_handoff = 0;
      release_client(s);
    }
    return status;
  }
}

static int ws_send(coro_socket_t *s, const char *data, size_t len) {
  return turbo_stream_send(s->handle.stream, data, len);
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
  return turbo_stream_send_buffer(s->handle.stream, buffer, len);
}

static void ws_close(coro_socket_t *s) {
  turbo_stream_t *stream = s->handle.stream;
  if (!stream || !s->owns_handle) {
    return;
  }
  s->handle.stream = NULL;
  retain_client(s);
  stream->on_close = on_ws_close;
  turbo_stream_close(stream);
}

const coro_transport_ops_t transport_ops_ws = {.connect = ws_connect,
                                               .send = ws_send,
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
  ws_connect_state_t *cfg;
  const char *actual_path;
  size_t path_len;
  size_t subprotocol_len = 0;

  if (!s || !host) {
    return socket_return_error(s, TURBO_EINVAL);
  }

  if (s->connected) {
    return socket_return_error(s, TURBO_EALREADY);
  }

  actual_path = (path && path[0] != '\0') ? path : "/";
  path_len = strlen(actual_path);
  if (path_len >= sizeof(((ws_connect_state_t *)0)->path)) {
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
  if (subprotocol_len > 0) {
    memcpy(cfg->subprotocol, subprotocol, subprotocol_len + 1);
  }
  cfg->is_tls = is_tls ? 1 : 0;

  ws_configure_socket(s);
  return coro_socket_connect(s, host, port);
}
