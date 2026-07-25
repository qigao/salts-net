/**
 * @file turbo_coro_socket_tcp.c
 * @brief TCP transport for coroutine sockets — delegates to turbo_stream_t.
 *
 * Mirrors the pipe transport pattern: coro layer owns a turbo_stream_t handle,
 * bridges callbacks to coro_socket_handle_transport_*, and yields/resumes.
 * All platform-specific I/O lives in the stream backend (IOCP/epoll/kqueue).
 */

#include "CoroNet/turbo_coro_internal.h"
#include "turbo_coro_send_profile_internal.h"
#include "turbo_stream_internal.h"
#include <stdlib.h>
#include <string.h>

extern const coro_transport_ops_t transport_ops_tcp;

#define CORO_TCP_EPOLL_SUBMIT_CHUNK_BYTES (64u * 1024u)

static int tcp_recv_start(coro_socket_t *s);

static int socket_ctx_error(coro_socket_t *s, int fallback) {
  return (s && s->ctx && s->ctx->last_error != 0) ? s->ctx->last_error : fallback;
}

/* ── Client callbacks ─────────────────────────────────────── */

static int on_tcp_recv(void *handle, const mem_slice_t *slice, void *peer) {
  UNUSED(peer);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  retain_client(s);
  coro_socket_handle_transport_recv(s, slice);
  release_client(s);
  return 0;
}

static void on_tcp_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  retain_client(s);
  coro_socket_handle_transport_connect(s, status);
  release_client(s);
}

static void on_tcp_write_complete(turbo_stream_t *stream, int status) {
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
#ifdef TURBO_CORONET_INTERNAL_PROFILING
  if (s->send_profile_active) s->send_profile_resume_signal_ns = turbo_hrtime();
#endif
  coro_resume_co(s->ctx, co);
  release_client(s);
}

static void on_tcp_close(void *handle) {
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

static void tcp_discard_stream(coro_socket_t *s) {
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

  /* Retry path: failed connect attempts must not reuse a dead stream handle. */
  if (s->handle.stream && !s->connected) {
    tcp_discard_stream(s);
  }

  /* Create stream handle on first connect */
  if (!s->handle.stream) {
    turbo_stream_kind_t kind = (sa->sa_family == AF_INET6)
                                   ? TURBO_STREAM_TCP6
                                   : TURBO_STREAM_TCP4;
    s->handle.stream = turbo_stream_create(s->ctx, kind);
    if (!s->handle.stream) return socket_ctx_error(s, TURBO_EIO);
    turbo_stream_set_user_data(s->handle.stream, s);
    turbo_stream_set_write_cb(s->handle.stream, on_tcp_write_complete);
    s->handle.stream->managed = 1;
    {
      int rc = coro_socket_apply_stream_options(s);
      if (rc != 0) {
        tcp_discard_stream(s);
        return rc;
      }
    }
  }

  retain_client(s);
  coro_set_wait(s);
  int r = turbo_stream_connect_addr(s->handle.stream, sa,
                                    on_tcp_connect, on_tcp_close);
  if (r != 0) {
    s->co_wait = NULL;
    release_client(s);
    tcp_discard_stream(s);
    return r;
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
      tcp_discard_stream(s);
    }
    return status;
  }
}

/* ── Bind/Listen/Accept ───────────────────────────────────── */

typedef struct tcp_accept_node_s {
  coro_socket_t *socket;
  struct tcp_accept_node_s *next;
} tcp_accept_node_t;

typedef struct tcp_listener_state_s {
  turbo_stream_listener_t *listener;
  coro_socket_t *server_coro;
  tcp_accept_node_t *head;
  tcp_accept_node_t *tail;
  int reuse_port;
} tcp_listener_state_t;

static void tcp_listener_fail(coro_socket_t *s, int status) {
  if (!s || status == 0) {
    return;
  }

  s->status = status;
  if (s->co_wait) {
    coro_resume_waiter(s);
  } else {
    s->accept_pending = 1;
  }
}

static void on_tcp_accept(void *listener_handle, void *stream_handle,
                          void *peer) {
  UNUSED(peer);
  turbo_stream_listener_t *l = (turbo_stream_listener_t *)listener_handle;
  tcp_listener_state_t *ls =
      (tcp_listener_state_t *)turbo_stream_listener_get_user_data(l);
  turbo_stream_t *stream = (turbo_stream_t *)stream_handle;
  coro_socket_t *child = NULL;
  if (!ls) {
    turbo_stream_destroy(stream);
    return;
  }

  tcp_accept_node_t *node = malloc(sizeof(tcp_accept_node_t));
  if (!node) {
    turbo_stream_destroy(stream);
    tcp_listener_fail(ls->server_coro, TURBO_ENOMEM);
    return;
  }

  child = coro_socket_create_shell(ls->server_coro->ctx, TURBO_TCP, &transport_ops_tcp);
  if (!child) {
    free(node);
    turbo_stream_destroy(stream);
    tcp_listener_fail(ls->server_coro, TURBO_ENOMEM);
    return;
  }

  child->handle.stream = stream;
  child->owns_handle = 1;
  child->accepted_ref = 1;
  retain_client(child);
  turbo_stream_set_user_data(stream, child);
  stream->on_recv = on_tcp_recv;
  stream->on_connect = on_tcp_connect;
  stream->on_close = on_tcp_close;
  stream->on_write_complete = on_tcp_write_complete;
  child->connected = 1;
  {
    int rc = coro_socket_inherit_stream_options(child, ls->server_coro);
    if (rc != 0) {
      coro_socket_destroy(child);
      free(node);
      tcp_listener_fail(ls->server_coro, rc);
      return;
    }
  }

  if (!ls->server_coro->accept_prestart_recv_disabled) {
    int rc = tcp_recv_start(child);
    if (rc != 0 && rc != TURBO_EALREADY) {
      coro_socket_destroy(child);
      free(node);
      tcp_listener_fail(ls->server_coro, rc);
      return;
    }
  }

  node->socket = child;
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

  ls->reuse_port = s->reuse_port;
  ls->listener = turbo_stream_listen_ex(s->ctx, kind, addr, backlog, on_tcp_accept,
                                        ls->reuse_port, ls);
  if (!ls->listener) {
    int rc = coro_context_get_last_error(s->ctx);
    return rc != 0 ? rc : TURBO_EIO;
  }
  if (s->tcp_keepalive_configured) {
    (void)turbo_stream_listener_set_child_tcp_keepalive(ls->listener,
                                                        &s->tcp_keepalive_config);
  }
  if (s->linger_configured) {
    (void)turbo_stream_listener_set_child_linger(ls->listener, &s->linger_config);
  }
  if (s->send_hwm_bytes) {
    (void)turbo_stream_listener_set_child_send_hwm(ls->listener, s->send_hwm_bytes);
  }
  if (s->socket_recv_buffer_bytes) {
    (void)turbo_stream_listener_set_child_recv_buffer_size(
        ls->listener, s->socket_recv_buffer_bytes);
  }
  if (s->socket_send_buffer_bytes) {
    (void)turbo_stream_listener_set_child_send_buffer_size(
        ls->listener, s->socket_send_buffer_bytes);
  }
  return 0;
}

static int tcp_accept(coro_socket_t *s, coro_socket_t **accepted) {
  tcp_listener_state_t *ls;

  retain_client(s);

  ls = (tcp_listener_state_t *)s->native_tcp_state;
  if (s->accept_pending) {
    s->accept_pending = 0;
  }
  if (!ls || !ls->head) {
    coro_set_wait(s);
    coro_yield();
    coro_socket_release_destroy_wait_handoff(s);
    if (s->status != 0) {
      int status = s->status;
      release_client(s);
      return status;
    }
    if (s->accept_pending) {
      s->accept_pending = 0;
    }
  }

  if (s->status != 0) {
    int status = s->status;
    release_client(s);
    return status;
  }

  ls = (tcp_listener_state_t *)s->native_tcp_state;
  if (!ls || !ls->head) {
    release_client(s);
    return TURBO_EBUSY;
  }

  tcp_accept_node_t *node = ls->head;
  ls->head = node->next;
  if (!ls->head) ls->tail = NULL;

  coro_socket_t *child = node->socket;
  free(node);

  *accepted = child;
  release_client(s);
  return 0;
}

/* ── Send/Recv ────────────────────────────────────────────── */

static int tcp_send_begin(coro_socket_t *s, coro_t **co, int *scheduled) {
  if (!s || !s->handle.stream || !co || !scheduled) return TURBO_EINVAL;
  *co = coro_running();
  *scheduled = *co ? coro_is_scheduled(*co) : 0;
  if (!*co) return TURBO_OK;
  s->write_status = 0;
  s->co_write_wait = *co;
#ifdef TURBO_CORONET_INTERNAL_PROFILING
  s->send_profile_active = *scheduled && turbo_coro_send_profile_enabled();
  s->send_profile_resume_signal_ns = 0u;
#endif
  if (*scheduled) coro_set_waiting_for_io(*co, 1);
  return TURBO_OK;
}

static int tcp_send_finish(coro_socket_t *s, coro_t *co, int scheduled, int submit_status) {
  if (!co) return submit_status;
  if (submit_status != TURBO_OK) {
    s->co_write_wait = NULL;
#ifdef TURBO_CORONET_INTERNAL_PROFILING
    s->send_profile_active = 0;
#endif
    if (scheduled) coro_set_waiting_for_io(co, 0);
    return submit_status;
  }
  while (s->co_write_wait == co) coro_yield();
#ifdef TURBO_CORONET_INTERNAL_PROFILING
  if (s->send_profile_active && s->send_profile_resume_signal_ns != 0u) {
    turbo_coro_send_profile_record_resume(s->send_profile_resume_signal_ns,
                                          s->ctx->send_profile_scheduler_entry_ns,
                                          turbo_hrtime());
  }
  s->send_profile_active = 0;
#endif
  return s->write_status;
}

static int tcp_send_one(coro_socket_t *s, const char *data, size_t len) {
  coro_t *co;
  int scheduled;
  int rc;

  rc = tcp_send_begin(s, &co, &scheduled);
  if (rc != TURBO_OK) return rc;
  rc = turbo_stream_send(s->handle.stream, data, len);
  return tcp_send_finish(s, co, scheduled, rc);
}

static int tcp_send(coro_socket_t *s, const char *data, size_t len) {
  size_t offset = 0u;
  int rc;

  if (!s || !data || len == 0u) return TURBO_EINVAL;
  if (coro_context_get_tcp_backend(s->ctx) != TURBO_TCP_BACKEND_EPOLL ||
      len <= CORO_TCP_EPOLL_SUBMIT_CHUNK_BYTES)
    return tcp_send_one(s, data, len);

  /* epoll owns a bounded SPSC submission ring.  Limiting one coroutine write
   * to less than that ring keeps cancellation on the coroutine boundary even
   * when a peer stops reading a multi-megabyte MQTT packet. */
  while (offset < len) {
    size_t chunk = len - offset;
    if (chunk > CORO_TCP_EPOLL_SUBMIT_CHUNK_BYTES)
      chunk = CORO_TCP_EPOLL_SUBMIT_CHUNK_BYTES;
    rc = tcp_send_one(s, data + offset, chunk);
    if (rc != TURBO_OK) return rc;
    offset += chunk;
  }
  return TURBO_OK;
}

static int tcp_sendv(coro_socket_t *s, const turbo_iovec_t *iov, size_t iovcnt) {
  mem_buffer_t *buffer = NULL;
  coro_t *co;
  size_t total = 0u;
  size_t offset = 0u;
  int scheduled;
  int rc;

  if (!s || !s->handle.stream || !iov || iovcnt == 0u) return TURBO_EINVAL;
  for (size_t i = 0u; i < iovcnt; ++i) {
    if (iov[i].len > 0u && !iov[i].data) return TURBO_EINVAL;
    if (iov[i].len > SIZE_MAX - total) return TURBO_ERANGE;
    total += iov[i].len;
  }
  if (total == 0u) return TURBO_EINVAL;
  if (coro_context_get_tcp_backend(s->ctx) == TURBO_TCP_BACKEND_EPOLL &&
      total > CORO_TCP_EPOLL_SUBMIT_CHUNK_BYTES) {
    buffer = turbo_stream_get_send_buffer(s->handle.stream, total);
    if (!buffer) return TURBO_ENOMEM;
    for (size_t i = 0u; i < iovcnt; ++i) {
      if (iov[i].len == 0u) continue;
      memcpy((char *)mem_buffer_data(buffer) + offset, iov[i].data, iov[i].len);
      offset += iov[i].len;
    }
    mem_set_used(buffer, total);
    rc = tcp_send(s, (const char *)mem_buffer_const_data(buffer), total);
    mem_unref(buffer);
    return rc;
  }
  rc = tcp_send_begin(s, &co, &scheduled);
  if (rc != TURBO_OK) return rc;
  if (co) {
    rc = turbo_stream_sendv_borrowed(s->handle.stream, iov, iovcnt, total);
    if (rc != TURBO_ENOTSUP) return tcp_send_finish(s, co, scheduled, rc);
  }
  buffer = turbo_stream_get_send_buffer(s->handle.stream, total);
  if (!buffer) return tcp_send_finish(s, co, scheduled, TURBO_ENOMEM);
  for (size_t i = 0u; i < iovcnt; ++i) {
    if (iov[i].len == 0u) continue;
    memcpy((char *)mem_buffer_data(buffer) + offset, iov[i].data, iov[i].len);
    offset += iov[i].len;
  }
  mem_set_used(buffer, total);
  rc = turbo_stream_send_buffer(s->handle.stream, buffer, total);
  mem_unref(buffer);
  return tcp_send_finish(s, co, scheduled, rc);
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
    if (ls->listener) {
      turbo_stream_listener_set_user_data(ls->listener, NULL);
      turbo_stream_listener_close(ls->listener);
    }
    if (s->co_wait) {
      s->accept_pending = 0;
      s->status = (s->status == 0) ? TURBO_ECANCELED : s->status;
      coro_resume_waiter(s);
    }
    tcp_accept_node_t *n = ls->head;
    while (n) {
      tcp_accept_node_t *nx = n->next;
      coro_socket_destroy(n->socket);
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
  stream->on_recv = NULL;
  stream->on_connect = NULL;
  stream->on_write_complete = NULL;
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
    .sendv = tcp_sendv,
    .recv_start = tcp_recv_start,
    .recv_stop = tcp_recv_stop,
    .get_local_addr = tcp_get_local_addr,
    .close = tcp_close,
    .get_send_buffer = tcp_get_send_buffer,
    .send_buffer = tcp_send_buffer};
