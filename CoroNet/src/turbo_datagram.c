/**
 * @file turbo_datagram.c
 * @brief Dispatch layer for turbo_datagram_t.
 *
 * Resolves the backend once at create time. Every public API call is a
 * direct function-pointer dispatch.
 */

#include "turbo_datagram_internal.h"
#include "turbo_buffer.h"
#include "internal.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_coro_internal.h"

#include <stdlib.h>
#include <string.h>

/* ── Backend resolution ───────────────────────────────────── */

static void datagram_record_error(coro_context_t *ctx, int err) {
  if (ctx) {
    ctx->last_error = err;
  }
}

static int datagram_kind_is_valid(turbo_datagram_kind_t kind) {
  switch (kind) {
  case TURBO_DATAGRAM_UDP4:
  case TURBO_DATAGRAM_UDP6:
    return 1;
  default:
    return 0;
  }
}

static void datagram_maybe_free(turbo_datagram_t *d) {
  if (!d || !d->closed) {
    return;
  }

  if (atomic_load_explicit(&d->ref_count, memory_order_acquire) == 0) {
    if (d->ops && d->ops->destroy_backend) {
      d->ops->destroy_backend(d);
    }
    free(d);
  }
}

static const turbo_datagram_backend_ops_t *datagram_platform_default_ops(void) {
#if defined(_WIN32)
  return &turbo_datagram_iocp_ops;
#elif defined(__linux__) && !defined(__ANDROID__) && TURBO_HAS_IO_URING
  return &turbo_datagram_io_uring_ops;
#elif defined(__linux__) || defined(__ANDROID__)
  return &turbo_datagram_epoll_ops;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  return &turbo_datagram_kqueue_ops;
#else
  return NULL;
#endif
}

static const turbo_datagram_backend_ops_t *datagram_udp_backend_ops(
    turbo_udp_backend_t backend) {
  switch (backend) {
  case TURBO_UDP_BACKEND_AUTO:
    return datagram_platform_default_ops();
#ifdef _WIN32
  case TURBO_UDP_BACKEND_IOCP:
    return &turbo_datagram_iocp_ops;
#elif defined(__linux__) && !defined(__ANDROID__) && TURBO_HAS_IO_URING
  case TURBO_UDP_BACKEND_IO_URING:
    return &turbo_datagram_io_uring_ops;
#elif defined(__linux__) || defined(__ANDROID__)
  case TURBO_UDP_BACKEND_EPOLL:
    return &turbo_datagram_epoll_ops;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  case TURBO_UDP_BACKEND_KQUEUE:
    return &turbo_datagram_kqueue_ops;
#endif
  default:
    return NULL;
  }
}

const turbo_datagram_backend_ops_t *turbo_datagram_resolve_backend(
    coro_context_t *ctx, turbo_datagram_kind_t kind) {
  if (!datagram_kind_is_valid(kind)) {
    return NULL;
  }
  return datagram_udp_backend_ops(ctx ? ctx->udp_backend : TURBO_UDP_BACKEND_AUTO);
}

/* ── Common init / teardown ───────────────────────────────── */

int turbo_datagram_init_common(turbo_datagram_t *d, coro_context_t *ctx,
                                turbo_datagram_kind_t kind,
                                const turbo_datagram_backend_ops_t *ops) {
  memset(d, 0, sizeof(*d));
  d->ctx = ctx;
  d->kind = kind;
  d->ops = ops;
  d->arena = (mem_pool_t *)coro_context_get_arena(ctx);
  if (!d->arena) return TURBO_ENOMEM;
  atomic_init(&d->ref_count, 1);
  d->status = 0;

  d->recv_buf[0] = mem_get_buffer(d->arena, 65536);
  d->recv_buf[1] = mem_get_buffer(d->arena, 65536);
  if (!d->recv_buf[0] || !d->recv_buf[1]) return TURBO_ENOMEM;

  return 0;
}

static void turbo_datagram_cleanup_create_failure(turbo_datagram_t *d, int native_ref_held) {
  if (!d) {
    return;
  }

  if (d->recv_buf[0]) {
    mem_unref(d->recv_buf[0]);
    d->recv_buf[0] = NULL;
  }
  if (d->recv_buf[1]) {
    mem_unref(d->recv_buf[1]);
    d->recv_buf[1] = NULL;
  }
  if (native_ref_held && d->ctx) {
    coro_context_native_unref(d->ctx);
  }
}

void turbo_datagram_finalize_close(turbo_datagram_t *d) {
  turbo_datagram_close_cb close_cb;
  void *close_cb_arg;

  if (d->recv_buf[0]) { mem_unref(d->recv_buf[0]); d->recv_buf[0] = NULL; }
  if (d->recv_buf[1]) { mem_unref(d->recv_buf[1]); d->recv_buf[1] = NULL; }
  d->connected = 0;
  d->status = 0;
  d->closing = 0;
  d->closed = 1;

  close_cb = d->close_cb;
  close_cb_arg = d->close_cb_arg;
  d->close_cb = NULL;
  d->close_cb_arg = NULL;
  if (close_cb) {
    close_cb(close_cb_arg);
  }

  if (d->native_ref_held) {
    d->native_ref_held = 0;
    coro_context_native_unref(d->ctx);
  }
  datagram_maybe_free(d);
}

void turbo_datagram_retain(turbo_datagram_t *d) {
  if (!d) return;
  atomic_fetch_add_explicit(&d->ref_count, 1, memory_order_relaxed);
}

void turbo_datagram_release(turbo_datagram_t *d) {
  if (!d) return;
  if (atomic_fetch_sub_explicit(&d->ref_count, 1, memory_order_acq_rel) == 1) {
    datagram_maybe_free(d);
  }
}

/* ── Public API: Lifecycle ────────────────────────────────── */

turbo_datagram_t *turbo_datagram_create(coro_context_t *ctx,
                                         turbo_datagram_kind_t kind) {
  int init_common_ok;

  if (!ctx) return NULL;

  const turbo_datagram_backend_ops_t *ops = turbo_datagram_resolve_backend(ctx, kind);
  if (!ops) {
    datagram_record_error(ctx, TURBO_EPROTONOSUPPORT);
    return NULL;
  }

  turbo_datagram_t *d = (turbo_datagram_t *)calloc(1, sizeof(turbo_datagram_t));
  if (!d) {
    datagram_record_error(ctx, TURBO_ENOMEM);
    return NULL;
  }

  init_common_ok = 0;
  int rc = turbo_datagram_init_common(d, ctx, kind, ops);
  if (rc != 0) {
    datagram_record_error(ctx, rc);
    turbo_datagram_cleanup_create_failure(d, init_common_ok);
    free(d);
    return NULL;
  }
  init_common_ok = 1;

  datagram_record_error(ctx, 0);
  return d;
}

void turbo_datagram_destroy(turbo_datagram_t *d) {
  if (!d) return;
  if (d->destroyed) return;
  d->destroyed = 1;

  if (d->closing) {
    /* Backend is still cleaning up. Let finalize_close() free it. */
    turbo_datagram_release(d);
    return;
  }

  /*
   * If not closing, we must initiate closure to ensure backend resources
   * (IOCP worker, handles) are cleaned up. finalize_close() will free d.
   */
  turbo_datagram_close(d);
  turbo_datagram_release(d);
}

/* ── Public API: Bind + Connect ───────────────────────────── */

int turbo_datagram_bind(turbo_datagram_t *d, const char *host,
                         unsigned short port) {
  int rc;

  if (!d) return TURBO_EINVAL;
  rc = d->ops->init(d, host, port);
  if (rc == 0 && !d->native_ref_held) {
    d->native_ref_held = 1;
    coro_context_native_ref(d->ctx);
  }
  return rc;
}

void turbo_datagram_set_reuse_port(turbo_datagram_t *d, int enable) {
  if (!d) return;
  d->reuse_port = enable ? 1 : 0;
}

int turbo_datagram_connect(turbo_datagram_t *d, const char *host,
                            unsigned short port) {
  if (!d) return TURBO_EINVAL;
  int rc = d->ops->connect(d, host, port);
  if (rc == 0) d->connected = 1;
  return rc;
}

/* ── Public API: Send ─────────────────────────────────────── */

int turbo_datagram_sendto(turbo_datagram_t *d, const struct sockaddr *dest,
                           const char *data, size_t len) {
  if (!d || !data || len == 0) return TURBO_EINVAL;
  mem_buffer_t *buf = mem_get_buffer(d->arena, len);
  if (!buf) return TURBO_ENOMEM;
  memcpy(buf->data, data, len);
  mem_set_used(buf, len);
  int rc = d->ops->send_buffer(d, dest, buf, len);
  mem_unref(buf);
  return rc;
}

int turbo_datagram_send(turbo_datagram_t *d, const char *data, size_t len) {
  return turbo_datagram_sendto(d, NULL, data, len);
}

mem_buffer_t *turbo_datagram_get_send_buffer(turbo_datagram_t *d,
                                              size_t min_size) {
  if (!d || !d->arena) return NULL;
  return mem_get_buffer(d->arena, min_size);
}

int turbo_datagram_sendto_buffer(turbo_datagram_t *d,
                                  const struct sockaddr *dest,
                                  mem_buffer_t *buf, size_t len) {
  if (!d || !buf) return TURBO_EINVAL;
  return d->ops->send_buffer(d, dest, buf, len);
}

int turbo_datagram_send_buffer(turbo_datagram_t *d, mem_buffer_t *buf,
                                size_t len) {
  return turbo_datagram_sendto_buffer(d, NULL, buf, len);
}

int turbo_datagram_sendv(turbo_datagram_t *d, const turbo_iovec_t *iov,
                          size_t iovcnt) {
  if (!d || !iov || iovcnt == 0) return TURBO_EINVAL;

  size_t total = 0;
  for (size_t i = 0; i < iovcnt; i++) total += iov[i].len;
  if (total == 0) return 0;

  mem_buffer_t *buf = turbo_datagram_get_send_buffer(d, total);
  if (!buf) return TURBO_ENOMEM;

  size_t offset = 0;
  for (size_t i = 0; i < iovcnt; i++) {
    memcpy(buf->data + offset, iov[i].data, iov[i].len);
    offset += iov[i].len;
  }
  mem_set_used(buf, total);

  int rc = turbo_datagram_send_buffer(d, buf, total);
  mem_unref(buf);
  return rc;
}

/* ── Public API: Receive ──────────────────────────────────── */

int turbo_datagram_recv_start(turbo_datagram_t *d, turbo_recv_cb on_recv) {
  if (!d || !on_recv) return TURBO_EINVAL;
  d->on_recv = on_recv;
  d->status = 0;
  if (!d->ops->recv_start) return 0;
  return d->ops->recv_start(d);
}

void turbo_datagram_recv_stop(turbo_datagram_t *d) {
  if (!d) return;
  if (!d->ops->recv_stop) return;
  d->ops->recv_stop(d);
}

/* ── Public API: Close ────────────────────────────────────── */

void turbo_datagram_close(turbo_datagram_t *d) {
  if (!d || d->closing || d->closed) return;
  d->closing = 1;
  d->ops->close(d);
}

/* ── Public API: Multicast / Broadcast ────────────────────── */

int turbo_datagram_join_multicast(turbo_datagram_t *d, const char *group,
                                   const char *iface) {
  if (!d || !group || group[0] == '\0') return TURBO_EINVAL;
  if (!d->ops->join_multicast) return TURBO_ENOTSUP;
  return d->ops->join_multicast(d, group, iface);
}

int turbo_datagram_leave_multicast(turbo_datagram_t *d, const char *group,
                                    const char *iface) {
  if (!d || !group || group[0] == '\0') return TURBO_EINVAL;
  if (!d->ops->leave_multicast) return TURBO_ENOTSUP;
  return d->ops->leave_multicast(d, group, iface);
}

int turbo_datagram_set_multicast_loop(turbo_datagram_t *d, int on) {
  if (!d) return TURBO_EINVAL;
  if (!d->ops->set_multicast_loop) return TURBO_ENOTSUP;
  return d->ops->set_multicast_loop(d, on);
}

int turbo_datagram_set_multicast_ttl(turbo_datagram_t *d, int ttl) {
  if (!d) return TURBO_EINVAL;
  if (ttl < 0 || ttl > 255) return TURBO_ERANGE;
  if (!d->ops->set_multicast_ttl) return TURBO_ENOTSUP;
  return d->ops->set_multicast_ttl(d, ttl);
}

int turbo_datagram_set_broadcast(turbo_datagram_t *d, int on) {
  if (!d) return TURBO_EINVAL;
  if (d->kind == TURBO_DATAGRAM_UDP6) return TURBO_ENOTSUP;
  if (!d->ops->set_broadcast) return TURBO_ENOTSUP;
  return d->ops->set_broadcast(d, on);
}

/* ── Public API: Query ────────────────────────────────────── */

int turbo_datagram_get_local_addr(turbo_datagram_t *d,
                                   struct sockaddr_storage *addr) {
  if (!d || !addr) return TURBO_EINVAL;
  return d->ops->get_local_addr(d, addr);
}

void turbo_datagram_set_user_data(turbo_datagram_t *d, void *data) {
  if (d) d->user_data = data;
}

void *turbo_datagram_get_user_data(turbo_datagram_t *d) {
  return d ? d->user_data : NULL;
}

void turbo_datagram_trim_memory(turbo_datagram_t *d) {
  if (d && d->arena) mem_trim(d->arena);
}
