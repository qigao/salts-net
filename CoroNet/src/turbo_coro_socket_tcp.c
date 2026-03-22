/**
 * @file turbo_coro_socket_tcp.c
 * @brief TCP transport for coroutine sockets — delegates to turbo_stream_t.
 *
 * Mirrors the pipe transport pattern: coro layer owns a turbo_stream_t handle,
 * bridges callbacks to coro_socket_handle_transport_*, and yields/resumes.
 * All platform-specific I/O lives in the stream backend (IOCP/epoll/kqueue).
 */

#include "CoroNet/turbo_coro_internal.h"
#include "turbo_stream_internal.h"
#include <stdlib.h>
#include <string.h>

extern const coro_transport_ops_t transport_ops_tcp;

/* ── Client callbacks ─────────────────────────────────────── */

static int on_tcp_recv(void *handle, const mem_slice_t *slice, void *peer) {
  UNUSED(peer);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  coro_socket_handle_transport_recv(s, slice);
  return 0;
}

static void on_tcp_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  coro_socket_handle_transport_connect(s, status);
}

static void on_tcp_close(void *handle) {
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  if (s) {
    turbo_stream_set_user_data(stream, NULL);
    coro_socket_handle_transport_close(s);
  }
}

/* ── Connect ──────────────────────────────────────────────── */

static int tcp_connect(coro_socket_t *s, const char *host, int port) {
  const char *ip = s->resolved_ip[0] ? s->resolved_ip : host;

  /* Build sockaddr from resolved IP */
  struct sockaddr_in addr4;
  struct sockaddr_in6 addr6;
  struct sockaddr *sa = NULL;

  memset(&addr4, 0, sizeof(addr4));
  memset(&addr6, 0, sizeof(addr6));

  if (inet_pton(AF_INET, ip, &addr4.sin_addr) == 1) {
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons((unsigned short)port);
    sa = (struct sockaddr *)&addr4;
  } else if (inet_pton(AF_INET6, ip, &addr6.sin6_addr) == 1) {
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons((unsigned short)port);
    sa = (struct sockaddr *)&addr6;
  }

  if (!sa) return TURBO_EINVAL;

  /* Create stream handle on first connect */
  if (!s->handle.stream) {
    turbo_stream_kind_t kind = (sa->sa_family == AF_INET6)
                                   ? TURBO_STREAM_TCP6
                                   : TURBO_STREAM_TCP4;
    s->handle.stream = turbo_stream_create(s->ctx, kind);
    if (!s->handle.stream) return TURBO_ENOMEM;
    turbo_stream_set_user_data(s->handle.stream, s);
    s->handle.stream->managed = 1;
  }

  retain_client(s);
  int r = turbo_stream_connect_addr(s->handle.stream, sa,
                                    on_tcp_connect, on_tcp_close);
  if (r != 0) {
    release_client(s);
    return r;
  }

  coro_set_wait(s);
  coro_yield();
  return s->status;
}

/* ── Bind/Listen/Accept ───────────────────────────────────── */

typedef struct tcp_accept_node_s {
  turbo_stream_t *stream;
  struct tcp_accept_node_s *next;
} tcp_accept_node_t;

typedef struct tcp_listener_state_s {
  turbo_stream_listener_t *listener;
  coro_socket_t *server_coro;
  tcp_accept_node_t *head;
  tcp_accept_node_t *tail;
  int reuse_port;
} tcp_listener_state_t;

static void on_tcp_accept(void *listener_handle, void *stream_handle,
                           void *peer) {
  UNUSED(peer);
  turbo_stream_listener_t *l = (turbo_stream_listener_t *)listener_handle;
  tcp_listener_state_t *ls =
      (tcp_listener_state_t *)turbo_stream_listener_get_user_data(l);
  if (!ls) return;

  tcp_accept_node_t *node = malloc(sizeof(tcp_accept_node_t));
  if (!node) {
    turbo_stream_destroy((turbo_stream_t *)stream_handle);
    return;
  }
  node->stream = (turbo_stream_t *)stream_handle;
  node->next = NULL;

  if (ls->tail)
    ls->tail->next = node;
  else
    ls->head = node;
  ls->tail = node;

  coro_socket_t *s = ls->server_coro;
  if (s->co_wait) {
    coro_resume_waiter(s);
  } else {
    s->accept_pending = 1;
  }
}

static int tcp_bind(coro_socket_t *s, const struct sockaddr *addr) {
  /* Store bind address for later use in tcp_listen */
  if (!s || !addr) return TURBO_EINVAL;

  tcp_listener_state_t *ls = (tcp_listener_state_t *)s->native_tcp_state;
  if (!ls) {
    ls = calloc(1, sizeof(tcp_listener_state_t));
    if (!ls) return TURBO_ENOMEM;
    ls->server_coro = s;
    ls->reuse_port = s->reuse_port;
    s->native_tcp_state = ls;
  }

  size_t addr_len = (addr->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6)
                                                   : sizeof(struct sockaddr_in);
  /* Store address in native_tcp_state for tcp_listen to use.
     We pack it after the listener_state struct. */
  void *new_ls = realloc(ls, sizeof(tcp_listener_state_t) + addr_len);
  if (!new_ls) return TURBO_ENOMEM;
  ls = (tcp_listener_state_t *)new_ls;
  s->native_tcp_state = ls;
  memcpy((char *)ls + sizeof(tcp_listener_state_t), addr, addr_len);

  return 0;
}

static int tcp_listen(coro_socket_t *s, int backlog) {
  tcp_listener_state_t *ls = (tcp_listener_state_t *)s->native_tcp_state;
  if (!ls) return TURBO_EINVAL;

  const struct sockaddr *addr =
      (const struct sockaddr *)((char *)ls + sizeof(tcp_listener_state_t));

  turbo_stream_kind_t kind = (addr->sa_family == AF_INET6)
                                 ? TURBO_STREAM_TCP6
                                 : TURBO_STREAM_TCP4;

  ls->listener = turbo_stream_listen(s->ctx, kind, addr, backlog, on_tcp_accept);
  if (!ls->listener) return TURBO_EADDRINUSE;

  turbo_stream_listener_set_user_data(ls->listener, ls);
  return 0;
}

static int tcp_accept(coro_socket_t *s, coro_socket_t **accepted) {
  retain_client(s);

  if (s->accept_pending) {
    s->accept_pending = 0;
  } else {
    coro_set_wait(s);
    coro_yield();
    if (s->status != 0) {
      int status = s->status;
      release_client(s);
      return status;
    }
  }

  tcp_listener_state_t *ls = (tcp_listener_state_t *)s->native_tcp_state;
  if (!ls || !ls->head) {
    release_client(s);
    return TURBO_EBUSY;
  }

  tcp_accept_node_t *node = ls->head;
  ls->head = node->next;
  if (!ls->head) ls->tail = NULL;

  turbo_stream_t *stream = node->stream;
  free(node);

  coro_socket_t *child =
      coro_socket_create_shell(s->ctx, TURBO_TCP, &transport_ops_tcp);
  if (!child) {
    turbo_stream_destroy(stream);
    release_client(s);
    return TURBO_ENOMEM;
  }

  child->handle.stream = stream;
  child->owns_handle = 1;
  turbo_stream_set_user_data(stream, child);
  stream->on_recv = on_tcp_recv;
  stream->on_connect = on_tcp_connect;
  stream->on_close = on_tcp_close;
  child->connected = 1;
  retain_client(child);

  *accepted = child;
  release_client(s);
  return 0;
}

/* ── Send/Recv ────────────────────────────────────────────── */

static int tcp_send(coro_socket_t *s, const char *data, size_t len) {
  return turbo_stream_send(s->handle.stream, data, len);
}

static mem_buffer_t *tcp_get_send_buffer(coro_socket_t *s, size_t min_size) {
  return turbo_stream_get_send_buffer(s->handle.stream, min_size);
}

static int tcp_send_buffer(coro_socket_t *s, mem_buffer_t *buffer, size_t len) {
  return turbo_stream_send_buffer(s->handle.stream, buffer, len);
}

static int tcp_recv_start(coro_socket_t *s) {
  return turbo_stream_recv_start(s->handle.stream, on_tcp_recv);
}

static void tcp_recv_stop(coro_socket_t *s) {
  turbo_stream_recv_stop(s->handle.stream);
}

/* ── Close ────────────────────────────────────────────────── */

static void tcp_close(coro_socket_t *s) {
  /* Listener close */
  if (s->native_tcp_state) {
    tcp_listener_state_t *ls = (tcp_listener_state_t *)s->native_tcp_state;
    if (ls->listener) turbo_stream_listener_close(ls->listener);
    tcp_accept_node_t *n = ls->head;
    while (n) {
      tcp_accept_node_t *nx = n->next;
      turbo_stream_destroy(n->stream);
      free(n);
      n = nx;
    }
    free(ls);
    s->native_tcp_state = NULL;
  }

  /* Client stream close */
  turbo_stream_t *stream = s->handle.stream;
  if (!stream || !s->owns_handle) return;
  s->handle.stream = NULL;
  s->close_pending = 1;
  retain_client(s);
  stream->on_close = on_tcp_close;
  turbo_stream_close(stream);
}

/* ── Address query ────────────────────────────────────────── */

static int tcp_get_local_addr(coro_socket_t *s, struct sockaddr_storage *addr) {
  if (s->handle.stream)
    return turbo_stream_get_local_addr(s->handle.stream, addr);

  /* Listener case */
  if (s->native_tcp_state) {
    tcp_listener_state_t *ls = (tcp_listener_state_t *)s->native_tcp_state;
    if (ls->listener) {
      /* Bind address is stored after the struct */
      const struct sockaddr *bind_addr =
          (const struct sockaddr *)((char *)ls +
                                    sizeof(tcp_listener_state_t));
      size_t len = (bind_addr->sa_family == AF_INET6)
                       ? sizeof(struct sockaddr_in6)
                       : sizeof(struct sockaddr_in);
      memset(addr, 0, sizeof(*addr));
      memcpy(addr, bind_addr, len);
      return 0;
    }
  }

  return TURBO_ENOTSUP;
}

/* ── Ops table ────────────────────────────────────────────── */

const coro_transport_ops_t transport_ops_tcp = {
    .connect = tcp_connect,
    .bind = tcp_bind,
    .listen = tcp_listen,
    .accept = tcp_accept,
    .send = tcp_send,
    .recv_start = tcp_recv_start,
    .recv_stop = tcp_recv_stop,
    .get_local_addr = tcp_get_local_addr,
    .close = tcp_close,
    .get_send_buffer = tcp_get_send_buffer,
    .send_buffer = tcp_send_buffer};
