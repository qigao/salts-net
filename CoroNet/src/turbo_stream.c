/**
 * @file turbo_stream.c
 * @brief Dispatch layer for turbo_stream_t.
 *
 * Resolves the backend once at create time, then every public API call is a
 * direct function-pointer dispatch. Zero branching, zero special cases.
 */

#include "turbo_stream_internal.h"
#include "turbo_build_config_internal.h"
#include "turbo_coro_internal.h"
#include "turbo_buffer.h"
#include "internal.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_coro_internal.h"

#include <stdlib.h>
#include <string.h>

/* ── Backend resolution ───────────────────────────────────── */

static int stream_has_prefix(const char *value, const char *prefix) {
  size_t prefix_len;

  if (!value || !prefix) {
    return 0;
  }

  prefix_len = strlen(prefix);
  return strncmp(value, prefix, prefix_len) == 0;
}

static char *stream_dup_cstr(const char *value) {
  size_t len;
  char *copy;

  if (!value) {
    return NULL;
  }

  len = strlen(value) + 1;
  copy = (char *)malloc(len);
  if (!copy) {
    return NULL;
  }

  memcpy(copy, value, len);
  return copy;
}

static char *stream_build_pipe_path(const char *prefix, const char *name, const char *suffix) {
  size_t prefix_len;
  size_t name_len;
  size_t suffix_len;
  char *path;

  if (!prefix || !name || !suffix || name[0] == '\0') {
    return NULL;
  }

  prefix_len = strlen(prefix);
  name_len = strlen(name);
  suffix_len = strlen(suffix);
  path = (char *)malloc(prefix_len + name_len + suffix_len + 1);
  if (!path) {
    return NULL;
  }

  memcpy(path, prefix, prefix_len);
  memcpy(path + prefix_len, name, name_len);
  memcpy(path + prefix_len + name_len, suffix, suffix_len);
  path[prefix_len + name_len + suffix_len] = '\0';
  return path;
}

static char *stream_normalize_pipe_name(const char *name) {
  const char *value;

  if (!name || name[0] == '\0') {
    return NULL;
  }

#ifdef _WIN32
  if (stream_has_prefix(name, "\\\\.\\pipe\\")) {
    return stream_dup_cstr(name);
  }

  value = name;
  if (stream_has_prefix(name, "pipe://")) {
    value = name + strlen("pipe://");
    while (*value == '/') {
      value++;
    }
  }

  if (value[0] == '\0') {
    return NULL;
  }

  return stream_build_pipe_path("\\\\.\\pipe\\", value, "");
#else
  if (stream_has_prefix(name, "ipc://")) {
    value = name + strlen("ipc://");
    if (value[0] == '\0') {
      return NULL;
    }
    return stream_dup_cstr(value);
  }

  if (stream_has_prefix(name, "pipe://")) {
    value = name + strlen("pipe://");
    if (value[0] == '\0') {
      return NULL;
    }
    if (value[0] == '/') {
      return stream_dup_cstr(value);
    }
    return stream_build_pipe_path("/tmp/", value, ".sock");
  }

  return stream_dup_cstr(name);
#endif
}

static void stream_record_error(coro_context_t *ctx, int err) {
  if (ctx) {
    ctx->last_error = err;
  }
}

static void turbo_stream_listener_cleanup_create_failure(turbo_stream_listener_t *l) {
  if (!l) {
    return;
  }

  if (l->backend_data && l->ops && l->ops->listener_close) {
    l->ops->listener_close(l);
    return;
  }

  free(l);
}

static int stream_kind_is_valid(turbo_stream_kind_t kind) {
  switch (kind) {
  case TURBO_STREAM_TCP4:
  case TURBO_STREAM_TCP6:
  case TURBO_STREAM_PIPE:
  case TURBO_STREAM_WS:
  case TURBO_STREAM_WSS:
  case TURBO_STREAM_TLS:
    return 1;
  default:
    return 0;
  }
}

static const turbo_stream_backend_ops_t *stream_platform_default_ops(void) {
#if defined(_WIN32)
  return &turbo_stream_iocp_ops;
#elif defined(__linux__) && !defined(__ANDROID__) && TURBO_HAS_IO_URING
  return &turbo_stream_io_uring_ops;
#elif defined(__linux__) || defined(__ANDROID__)
  return &turbo_stream_epoll_ops;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  return &turbo_stream_kqueue_ops;
#else
  return NULL;
#endif
}

static const turbo_stream_backend_ops_t *stream_tcp_backend_ops(
    turbo_tcp_backend_t backend) {
  switch (backend) {
  case TURBO_TCP_BACKEND_AUTO:
    return stream_platform_default_ops();
#ifdef _WIN32
  case TURBO_TCP_BACKEND_IOCP:
    return &turbo_stream_iocp_ops;
#elif defined(__linux__) && !defined(__ANDROID__) && TURBO_HAS_IO_URING
  case TURBO_TCP_BACKEND_IO_URING:
    return &turbo_stream_io_uring_ops;
#elif defined(__linux__) || defined(__ANDROID__)
  case TURBO_TCP_BACKEND_EPOLL:
    return &turbo_stream_epoll_ops;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  case TURBO_TCP_BACKEND_KQUEUE:
    return &turbo_stream_kqueue_ops;
#endif
  default:
    return NULL;
  }
}

const turbo_stream_backend_ops_t *turbo_stream_resolve_backend(
    coro_context_t *ctx, turbo_stream_kind_t kind) {
  if (!stream_kind_is_valid(kind)) {
    return NULL;
  }
  if (kind == TURBO_STREAM_PIPE) {
#ifdef _WIN32
    return &turbo_stream_pipe_win_ops;
#else
    return &turbo_stream_pipe_unix_ops;
#endif
  }
  if (kind == TURBO_STREAM_WS || kind == TURBO_STREAM_WSS) {
    return &turbo_stream_ws_ops;
  }
  if (kind == TURBO_STREAM_TLS) {
    return &turbo_stream_tls_ops;
  }
  return stream_tcp_backend_ops(ctx ? ctx->tcp_backend : TURBO_TCP_BACKEND_AUTO);
}

/* ── Common init / teardown ───────────────────────────────── */

int turbo_stream_init_common(turbo_stream_t *s, coro_context_t *ctx,
                              turbo_stream_kind_t kind,
                              const turbo_stream_backend_ops_t *ops) {
  memset(s, 0, sizeof(*s));
  s->ctx = ctx;
  s->kind = kind;
  s->ops = ops;
  s->arena = (mem_pool_t *)coro_context_get_arena(ctx);
  if (!s->arena) return TURBO_ENOMEM;

  s->recv_buf[0] = mem_get_buffer(s->arena, 65536);
  s->recv_buf[1] = mem_get_buffer(s->arena, 65536);
  if (!s->recv_buf[0] || !s->recv_buf[1]) return TURBO_ENOMEM;

  coro_context_native_ref(ctx);
  return 0;
}

static void turbo_stream_cleanup_create_failure(turbo_stream_t *s, int native_ref_held) {
  if (!s) {
    return;
  }

  if (s->recv_buf[0]) {
    mem_unref(s->recv_buf[0]);
    s->recv_buf[0] = NULL;
  }
  if (s->recv_buf[1]) {
    mem_unref(s->recv_buf[1]);
    s->recv_buf[1] = NULL;
  }
  if (native_ref_held && s->ctx) {
    coro_context_native_unref(s->ctx);
  }
}

void turbo_stream_enqueue_buffer(turbo_stream_t *s, mem_buffer_t *buf) {
  mem_ref(buf);
  buf->next = NULL;
  if (s->send_tail) {
    s->send_tail->next = buf;
  } else {
    s->send_head = buf;
  }
  s->send_tail = buf;
  s->send_queued += buf->used;
}

static void drain_send_queue(turbo_stream_t *s) {
  mem_buffer_t *cur = s->send_head;
  while (cur) {
    mem_buffer_t *next = cur->next;
    mem_unref(cur);
    cur = next;
  }
  s->send_head = NULL;
  s->send_tail = NULL;
  s->send_queued = 0;
}

void turbo_stream_finalize_close(turbo_stream_t *s) {
  if (!s || s->finalized) {
    return;
  }

  s->finalized = 1;
  drain_send_queue(s);
  if (s->recv_buf[0]) { mem_unref(s->recv_buf[0]); s->recv_buf[0] = NULL; }
  if (s->recv_buf[1]) { mem_unref(s->recv_buf[1]); s->recv_buf[1] = NULL; }
  s->connected = 0;
  s->closing = 0;

  if (s->on_close) {
    s->on_close(s);
  }

  coro_context_native_unref(s->ctx);

  if (s->destroyed && !s->managed) {
    free(s);
  }
}

void turbo_stream_listener_finalize_close(turbo_stream_listener_t *l) {
  if (l->backend_data) {
    /* Backend should have cleaned up already */
    l->backend_data = NULL;
  }
  free(l);
}

/* ── Public API: Lifecycle ────────────────────────────────── */

turbo_stream_t *turbo_stream_create(coro_context_t *ctx,
                                     turbo_stream_kind_t kind) {
  const turbo_stream_backend_ops_t *ops;
  turbo_stream_t *s;
  int rc;
  int native_ref_held;

  if (!ctx) return NULL;

  ops = turbo_stream_resolve_backend(ctx, kind);
  if (!ops) {
    stream_record_error(ctx, TURBO_EPROTONOSUPPORT);
    return NULL;
  }

  s = (turbo_stream_t *)calloc(1, sizeof(turbo_stream_t));
  if (!s) {
    stream_record_error(ctx, TURBO_ENOMEM);
    return NULL;
  }

  native_ref_held = 0;
  rc = turbo_stream_init_common(s, ctx, kind, ops);
  if (rc != 0) {
    goto fail;
  }
  native_ref_held = 1;

  if (ops->init) {
    rc = ops->init(s);
    if (rc != 0) {
      goto fail;
    }
  }

  stream_record_error(ctx, 0);
  return s;

fail:
  stream_record_error(ctx, rc);
  turbo_stream_cleanup_create_failure(s, native_ref_held);
  free(s);
  return NULL;
}

void turbo_stream_destroy(turbo_stream_t *s) {
  if (!s) return;
  s->destroyed = 1;

  if (s->finalized) {
    if (!s->managed) {
      free(s);
    }
    return;
  }

  if (s->closing) {
    /* Backend is still cleaning up. Let finalize_close() free it. */
    return;
  }

  if (s->backend_data) {
    /* Backend still owns resources or a worker thread. Close asynchronously. */
    turbo_stream_close(s);
    return;
  }

  /* No backend state was ever created: safe to finalize synchronously. */
  drain_send_queue(s);
  turbo_stream_finalize_close(s);
}

/* ── Public API: Connection ───────────────────────────────── */

int turbo_stream_connect(turbo_stream_t *s, const char *host,
                          unsigned short port, turbo_connect_cb on_connect,
                          turbo_close_cb on_close) {
  if (!s || !host) return TURBO_EINVAL;
  s->on_connect = on_connect;
  s->on_close = on_close;

  /* Resolve hostname to sockaddr, then delegate to backend */
  struct sockaddr_in addr4;
  struct sockaddr_in6 addr6;
  struct sockaddr *sa = NULL;

  memset(&addr4, 0, sizeof(addr4));
  memset(&addr6, 0, sizeof(addr6));

  if (s->kind == TURBO_STREAM_TCP4 || s->kind == TURBO_STREAM_TCP6 ||
      s->kind == TURBO_STREAM_TLS || s->kind == TURBO_STREAM_WS || s->kind == TURBO_STREAM_WSS) {
    /* Try direct IP parse first */
    if (inet_pton(AF_INET, host, &addr4.sin_addr) == 1) {
      addr4.sin_family = AF_INET;
      addr4.sin_port = htons(port);
      sa = (struct sockaddr *)&addr4;
    } else if (inet_pton(AF_INET6, host, &addr6.sin6_addr) == 1) {
      addr6.sin6_family = AF_INET6;
      addr6.sin6_port = htons(port);
      sa = (struct sockaddr *)&addr6;
    }
  }

  if (sa) {
    return s->ops->connect(s, sa);
  }

  /* DNS resolution needed — backend handles it or we do it here.
     For now, return error; DNS integration comes with coro layer. */
  return TURBO_EINVAL;
}

int turbo_stream_connect_addr(turbo_stream_t *s, const struct sockaddr *addr,
                               turbo_connect_cb on_connect,
                               turbo_close_cb on_close) {
  if (!s || !addr) return TURBO_EINVAL;
  s->on_connect = on_connect;
  s->on_close = on_close;
  return s->ops->connect(s, addr);
}

int turbo_stream_connect_pipe(turbo_stream_t *s, const char *name,
                               turbo_connect_cb on_connect,
                               turbo_close_cb on_close) {
  char *native_name;
  int rc;

  if (!s || !name) return TURBO_EINVAL;
  if (s->kind != TURBO_STREAM_PIPE) return TURBO_EINVAL;
  s->on_connect = on_connect;
  s->on_close = on_close;
  if (!s->ops->connect_pipe) return TURBO_ENOTSUP;

  native_name = stream_normalize_pipe_name(name);
  if (!native_name) {
    return TURBO_EINVAL;
  }

  rc = s->ops->connect_pipe(s, native_name);
  free(native_name);
  return rc;
}

/* ── Public API: Send ─────────────────────────────────────── */

int turbo_stream_send(turbo_stream_t *s, const char *data, size_t len) {
  if (!s || !data || len == 0) return TURBO_EINVAL;

  if (s->ops->send) {
    return s->ops->send(s, data, len);
  }

  mem_buffer_t *buf = mem_get_buffer(s->arena, len);
  if (!buf) return TURBO_ENOMEM;

  memcpy(buf->data, data, len);
  mem_set_used(buf, len);

  int rc = turbo_stream_send_buffer(s, buf, len);
  mem_unref(buf);
  return rc;
}

mem_buffer_t *turbo_stream_get_send_buffer(turbo_stream_t *s,
                                            size_t min_size) {
  if (!s) return NULL;
  return mem_get_buffer(s->arena, min_size);
}

int turbo_stream_send_buffer(turbo_stream_t *s, mem_buffer_t *buf,
                              size_t len) {
  if (!s || !buf) return TURBO_EINVAL;
  mem_set_used(buf, len);
  turbo_stream_enqueue_buffer(s, buf);
  return turbo_stream_flush(s);
}

int turbo_stream_flush(turbo_stream_t *s) {
  mem_buffer_t *buf;
  int rc;

  if (!s) return TURBO_EINVAL;
  if (!s->send_head) return 0;
  if (s->ops->flush) return s->ops->flush(s);
  if (!s->ops->send) return TURBO_ENOTSUP;

  while ((buf = s->send_head) != NULL) {
    rc = s->ops->send(s, buf->data, buf->used);
    if (rc != 0) return rc;

    s->send_head = buf->next;
    if (!s->send_head) s->send_tail = NULL;
    s->send_queued -= buf->used;
    mem_unref(buf);
  }

  return 0;
}

/* ── Public API: Receive ──────────────────────────────────── */

int turbo_stream_recv_start(turbo_stream_t *s, turbo_recv_cb on_recv) {
  if (!s || !on_recv) return TURBO_EINVAL;
  s->on_recv = on_recv;
  if (!s->ops->recv_start) return 0;
  return s->ops->recv_start(s);
}

void turbo_stream_recv_stop(turbo_stream_t *s) {
  if (!s) return;
  if (!s->ops->recv_stop) return;
  s->ops->recv_stop(s);
}

/* ── Public API: Close ────────────────────────────────────── */

void turbo_stream_close(turbo_stream_t *s) {
  if (!s || s->closing) return;
  s->closing = 1;
  s->ops->close(s);
}

/* ── Public API: Listener ─────────────────────────────────── */

turbo_stream_listener_t *turbo_stream_listen(
    coro_context_t *ctx, turbo_stream_kind_t kind,
    const struct sockaddr *addr, int backlog, turbo_accept_cb on_accept) {
  turbo_stream_listener_t *l;
  const turbo_stream_backend_ops_t *ops;
  int rc;

  if (!ctx || !addr || !on_accept) {
    stream_record_error(ctx, TURBO_EINVAL);
    return NULL;
  }

  l = (turbo_stream_listener_t *)calloc(1, sizeof(turbo_stream_listener_t));
  if (!l) {
    stream_record_error(ctx, TURBO_ENOMEM);
    return NULL;
  }

  l->ctx = ctx;
  l->kind = kind;
  ops = turbo_stream_resolve_backend(ctx, kind);
  if (!ops || !ops->bind || !ops->listen) {
    stream_record_error(ctx, TURBO_EPROTONOSUPPORT);
    turbo_stream_listener_cleanup_create_failure(l);
    return NULL;
  }
  l->ops = ops;
  l->arena = ctx->arena;
  l->on_accept = on_accept;

  rc = ops->bind(l, addr);
  if (rc != 0) {
    stream_record_error(ctx, rc);
    turbo_stream_listener_cleanup_create_failure(l);
    return NULL;
  }

  rc = ops->listen(l, backlog);
  if (rc != 0) {
    stream_record_error(ctx, rc);
    turbo_stream_listener_cleanup_create_failure(l);
    return NULL;
  }

  stream_record_error(ctx, 0);
  return l;
}

turbo_stream_listener_t *turbo_stream_listen_pipe(
    coro_context_t *ctx, const char *name, int backlog,
    turbo_accept_cb on_accept) {
  const turbo_stream_backend_ops_t *ops;
  turbo_stream_listener_t *l;
  char *native_name;
  int rc;

  if (!ctx || !name || !on_accept) {
    stream_record_error(ctx, TURBO_EINVAL);
    return NULL;
  }

  ops = turbo_stream_resolve_backend(ctx, TURBO_STREAM_PIPE);
  if (!ops || !ops->bind_pipe || !ops->listen) {
    stream_record_error(ctx, TURBO_EPROTONOSUPPORT);
    return NULL;
  }

  l = (turbo_stream_listener_t *)calloc(1, sizeof(turbo_stream_listener_t));
  if (!l) {
    stream_record_error(ctx, TURBO_ENOMEM);
    return NULL;
  }

  l->ctx = ctx;
  l->kind = TURBO_STREAM_PIPE;
  l->ops = ops;
  l->arena = ctx->arena;
  l->on_accept = on_accept;

  native_name = stream_normalize_pipe_name(name);
  if (!native_name) {
    stream_record_error(ctx, TURBO_EINVAL);
    turbo_stream_listener_cleanup_create_failure(l);
    return NULL;
  }

  rc = ops->bind_pipe(l, native_name);
  free(native_name);
  if (rc != 0) {
    stream_record_error(ctx, rc);
    turbo_stream_listener_cleanup_create_failure(l);
    return NULL;
  }

  rc = ops->listen(l, backlog);
  if (rc != 0) {
    stream_record_error(ctx, rc);
    turbo_stream_listener_cleanup_create_failure(l);
    return NULL;
  }

  stream_record_error(ctx, 0);
  return l;
}

void turbo_stream_listener_close(turbo_stream_listener_t *l) {
  if (!l) return;
  l->ops->listener_close(l);
}

void turbo_stream_listener_set_user_data(turbo_stream_listener_t *l, void *data) {
  if (l) l->user_data = data;
}

void *turbo_stream_listener_get_user_data(turbo_stream_listener_t *l) {
  return l ? l->user_data : NULL;
}

/* ── Public API: Query ────────────────────────────────────── */

int turbo_stream_get_local_addr(turbo_stream_t *s,
                                 struct sockaddr_storage *addr) {
  if (!s || !addr) return TURBO_EINVAL;
  if (!s->ops->get_local_addr) return TURBO_ENOTSUP;
  return s->ops->get_local_addr(s, addr);
}

int turbo_stream_get_peer_addr(turbo_stream_t *s,
                                struct sockaddr_storage *addr) {
  if (!s || !addr) return TURBO_EINVAL;
  if (!s->ops->get_peer_addr) return TURBO_ENOTSUP;
  return s->ops->get_peer_addr(s, addr);
}

void turbo_stream_set_user_data(turbo_stream_t *s, void *data) {
  if (s) s->user_data = data;
}

void *turbo_stream_get_user_data(turbo_stream_t *s) {
  return s ? s->user_data : NULL;
}

void turbo_stream_set_write_cb(turbo_stream_t *s,
                                turbo_stream_write_cb cb) {
  if (s) s->on_write_complete = cb;
}
