/**
 * @file turbo_coro_socket_pipe.c
 * @brief Named pipe transport implementation using turbo_stream_t.
 */

#include "CoroNet/turbo_coro_internal.h"
#include "turbo_stream_internal.h"
#include <stdlib.h>

extern const coro_transport_ops_t transport_ops_pipe;

static int socket_ctx_error(coro_socket_t *s, int fallback) {
  return (s && s->ctx && s->ctx->last_error != 0) ? s->ctx->last_error : fallback;
}

/* ── Client callbacks ─────────────────────────────────────── */

static int on_pipe_recv(void *handle, const mem_slice_t *slice, void *peer) {
  UNUSED(peer);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  coro_socket_handle_transport_recv(s, slice);
  return 0;
}

static void on_pipe_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  coro_socket_handle_transport_connect(s, status);
}

static void on_pipe_close(void *handle) {
  turbo_stream_t *stream = (turbo_stream_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_stream_get_user_data(stream);
  if (s) {
    turbo_stream_set_user_data(stream, NULL);
    coro_socket_handle_transport_close(s);
  }
}

static void pipe_discard_stream(coro_socket_t *s) {
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

static void pipe_listener_fail(coro_socket_t *s, int status) {
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

/* ── Connect ──────────────────────────────────────────────── */

static int pipe_connect(coro_socket_t *s, const char *path, int port) {
  UNUSED(port);

  if (s->handle.stream && !s->connected) {
    pipe_discard_stream(s);
  }

  if (!s->handle.stream) {
    s->handle.stream = turbo_stream_create(s->ctx, TURBO_STREAM_PIPE);
    if (!s->handle.stream) {
      return socket_ctx_error(s, TURBO_EIO);
    }
    turbo_stream_set_user_data(s->handle.stream, s);
    s->handle.stream->managed = 1;
  }

  retain_client(s);
  int r = turbo_stream_connect_pipe(s->handle.stream, path,
                                    on_pipe_connect, on_pipe_close);
  if (r != 0) {
    release_client(s);
    pipe_discard_stream(s);
    return r;
  }
  coro_set_wait(s);
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

/* ── Bind/Listen/Accept ───────────────────────────────────── */

/* Pending accepted stream queued before coro wakes */
typedef struct pipe_accept_node_s {
  turbo_stream_t *stream;
  struct pipe_accept_node_s *next;
} pipe_accept_node_t;

typedef struct pipe_listener_state_s {
  turbo_stream_listener_t *listener;
  coro_socket_t           *server_coro;
  pipe_accept_node_t      *head;
  pipe_accept_node_t      *tail;
} pipe_listener_state_t;

static void on_pipe_accept(void *listener_handle, void *stream_handle, void *peer) {
  UNUSED(peer);
  turbo_stream_listener_t *l = (turbo_stream_listener_t *)listener_handle;
  pipe_listener_state_t *ls = (pipe_listener_state_t *)turbo_stream_listener_get_user_data(l);
  if (!ls) return;

  pipe_accept_node_t *node = malloc(sizeof(pipe_accept_node_t));
  if (!node) {
    turbo_stream_destroy((turbo_stream_t *)stream_handle);
    pipe_listener_fail(ls->server_coro, TURBO_ENOMEM);
    return;
  }
  node->stream = (turbo_stream_t *)stream_handle;
  node->next = NULL;

  if (ls->tail) ls->tail->next = node; else ls->head = node;
  ls->tail = node;

  coro_socket_t *s = ls->server_coro;
  if (s->co_wait) {
    coro_resume_waiter(s);
  } else {
    s->accept_pending = 1;
  }
}

static int pipe_bind(coro_socket_t *s, const struct sockaddr *addr) {
  UNUSED(s); UNUSED(addr);
  return TURBO_ENOTSUP;
}

static int pipe_listen(coro_socket_t *s, int backlog) {
  /* path stored in s->peer_addr as a string via listen_pipe */
  const char *path = (const char *)s->native_tcp_state;
  if (!path) return TURBO_EINVAL;

  pipe_listener_state_t *ls = malloc(sizeof(pipe_listener_state_t));
  if (!ls) return TURBO_ENOMEM;
  ls->server_coro = s;
  ls->head = ls->tail = NULL;

  ls->listener = turbo_stream_listen_pipe(s->ctx, path, backlog, on_pipe_accept);
  if (!ls->listener) {
    int rc = coro_context_get_last_error(s->ctx);
    free(ls);
    return rc != 0 ? rc : TURBO_EIO;
  }

  turbo_stream_listener_set_user_data(ls->listener, ls);
  /* replace native_tcp_state with the listener state */
  free(s->native_tcp_state);
  s->native_tcp_state = ls;
  return 0;
}

static int pipe_accept(coro_socket_t *s, coro_socket_t **accepted) {
  retain_client(s);

  if (s->accept_pending) {
    s->accept_pending = 0;
  } else {
    coro_set_wait(s);
    coro_yield();
    if (s->status != 0) {
      release_client(s);
      return s->status;
    }
  }

  if (s->status != 0) {
    int status = s->status;
    release_client(s);
    return status;
  }

  pipe_listener_state_t *ls = (pipe_listener_state_t *)s->native_tcp_state;
  if (!ls || !ls->head) { release_client(s); return TURBO_EBUSY; }

  pipe_accept_node_t *node = ls->head;
  ls->head = node->next;
  if (!ls->head) ls->tail = NULL;

  turbo_stream_t *stream = node->stream;
  free(node);

  coro_socket_t *child = coro_socket_create_shell(s->ctx, TURBO_PIPE,
                                                   &transport_ops_pipe);
  if (!child) {
    turbo_stream_destroy(stream);
    release_client(s);
    return TURBO_ENOMEM;
  }

  child->handle.stream = stream;
  child->owns_handle = 1;
  turbo_stream_set_user_data(stream, child);
  stream->on_recv    = on_pipe_recv;
  stream->on_connect = on_pipe_connect;
  stream->on_close   = on_pipe_close;
  child->connected = 1;
  retain_client(child);

  *accepted = child;
  release_client(s);
  return 0;
}

/* ── Send/Recv ────────────────────────────────────────────── */

static int pipe_send(coro_socket_t *s, const char *data, size_t len) {
  return turbo_stream_send(s->handle.stream, data, len);
}

static int pipe_recv_start(coro_socket_t *s) {
  return turbo_stream_recv_start(s->handle.stream, on_pipe_recv);
}

static void pipe_recv_stop(coro_socket_t *s) {
  turbo_stream_recv_stop(s->handle.stream);
}

/* ── Close ────────────────────────────────────────────────── */

static void pipe_close(coro_socket_t *s) {
  /* Listener close */
  if (s->native_tcp_state) {
    pipe_listener_state_t *ls = (pipe_listener_state_t *)s->native_tcp_state;
    if (ls->listener) turbo_stream_listener_close(ls->listener);
    pipe_accept_node_t *n = ls->head;
    while (n) {
      pipe_accept_node_t *nx = n->next;
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
  retain_client(s);
  stream->on_close = on_pipe_close;
  turbo_stream_close(stream);
}

/* ── Ops table ────────────────────────────────────────────── */

const coro_transport_ops_t transport_ops_pipe = {
    .connect        = pipe_connect,
    .bind           = pipe_bind,
    .listen         = pipe_listen,
    .accept         = pipe_accept,
    .send           = pipe_send,
    .recv_start     = pipe_recv_start,
    .recv_stop      = pipe_recv_stop,
    .get_local_addr = NULL,
    .close          = pipe_close
};
