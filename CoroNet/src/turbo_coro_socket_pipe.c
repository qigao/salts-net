/**
 * @file turbo_coro_socket_pipe.c
 * @brief Unix domain socket (Pipe) transport implementation.
 */

#include "CoroNet/turbo_coro_internal.h"
#include <stdlib.h>

/* ── Pipe Connect ─────────────────────────────────────────── */

static int on_pipe_coro_recv(void *handle, const mem_slice_t *slice, void *peer) {
  UNUSED(peer);
  turbo_pipe_client_t *pipe = (turbo_pipe_client_t *)handle;
  coro_socket_t *s = (coro_socket_t *)pipe->user_data;
  coro_socket_handle_transport_recv(s, slice);
  return 0;
}

static void on_pipe_coro_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_pipe_client_t *pipe = (turbo_pipe_client_t *)handle;
  coro_socket_t *s = (coro_socket_t *)pipe->user_data;
  coro_socket_handle_transport_connect(s, status);
}

static void on_pipe_coro_close(void *handle) {
  turbo_pipe_client_t *pipe = (turbo_pipe_client_t *)handle;
  coro_socket_t *s = (coro_socket_t *)pipe->user_data;
  if (s) {
    pipe->user_data = NULL;
    coro_socket_handle_transport_close(s);
  }
}

static int pipe_connect(coro_socket_t *s, const char *path, int port) {
  UNUSED(port);
  
  retain_client(s);
  int r = turbo_pipe_client_connect(
      s->handle.pipe, 
      path,
      on_pipe_coro_recv, 
      on_pipe_coro_connect, 
      on_pipe_coro_close
  );
  
  if (r != 0) {
    release_client(s);
    return r;
  }
  
  coro_set_wait(s);
  coro_yield();
  return s->status;
}

/* ── Pipe Bind/Listen/Accept ──────────────────────────────── */

static int pipe_bind(coro_socket_t *s, const struct sockaddr *addr) {
  UNUSED(s);
  UNUSED(addr);
  return UV_ENOSYS; /* Pipe uses path-based bind */
}

static void on_pipe_listen(uv_stream_t *stream, int status) {
  turbo_pipe_client_t *pipe = (turbo_pipe_client_t *)stream->data;
  coro_socket_t *s = (coro_socket_t *)pipe->user_data;
  if (!s) return;
  
  s->status = status;
  if (s->co_wait) {
    coro_resume_waiter(s);
  } else {
    s->accept_pending = 1;
  }
}

static int pipe_listen(coro_socket_t *s, int backlog) {
  return uv_listen(
      (uv_stream_t *)&s->handle.pipe->handle, 
      backlog, 
      on_pipe_listen
  );
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
  
  coro_socket_t *child = coro_socket_create(s->ctx, CORO_SOCKET_PIPE);
  if (!child) {
    release_client(s);
    return UV_ENOMEM;
  }
  
  int r = uv_accept(
      (uv_stream_t *)&s->handle.pipe->handle, 
      (uv_stream_t *)&child->handle.pipe->handle
  );
  
  if (r != 0) {
    coro_socket_destroy(child);
    release_client(s);
    return r;
  }
  
  /* Set callbacks explicitly - listener socket doesn't receive data,
   * so we can't copy its NULL callbacks. Use the bridge functions. */
  child->handle.pipe->on_recv = on_pipe_coro_recv;
  child->handle.pipe->on_connect = on_pipe_coro_connect;
  child->handle.pipe->on_close = on_pipe_coro_close;
  child->handle.pipe->user_data = child;
  child->connected = 1;
  retain_client(child);
  
  *accepted = child;
  release_client(s);
  return 0;
}

/* ── Pipe Send/Recv ───────────────────────────────────────── */

static int pipe_send(coro_socket_t *s, const char *data, size_t len) {
  return turbo_pipe_send(s->handle.pipe, data, len);
}

static int pipe_recv_start(coro_socket_t *s) {
  return turbo_pipe_read_start(s->handle.pipe);
}

static void pipe_recv_stop(coro_socket_t *s) {
  turbo_pipe_read_stop(s->handle.pipe);
}

/* ── Pipe Close ───────────────────────────────────────────── */

static void pipe_close(coro_socket_t *s) {
  turbo_pipe_client_t *pipe = s->handle.pipe;
  if (!pipe || !s->owns_handle) return;
  
  s->handle.pipe = NULL; /* Atomic clear to prevent double-close */
  
  if (!pipe->closing) {
    retain_client(s);
    pipe->on_close = on_pipe_coro_close;
    turbo_pipe_client_close(pipe);
  }
}

/* ── Pipe Transport Ops ───────────────────────────────────── */

const coro_transport_ops_t transport_ops_pipe = {
    .connect = pipe_connect,
    .bind = pipe_bind,
    .listen = pipe_listen,
    .accept = pipe_accept,
    .send = pipe_send,
    .recv_start = pipe_recv_start,
    .recv_stop = pipe_recv_stop,
    .get_local_addr = NULL,
    .close = pipe_close
};
