/**
 * @file turbo_datagram_internal.h
 * @brief Internal struct layout and backend vtable for turbo_datagram_t.
 *
 * @warning Internal header — not part of the public API.
 */

#ifndef TURBO_DATAGRAM_INTERNAL_H
#define TURBO_DATAGRAM_INTERNAL_H

#include "CoroNet/turbo_datagram.h"
#include "CoroNet/turbo_coro_context.h"
#include "turbo_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Backend vtable ───────────────────────────────────────── */

typedef struct turbo_datagram_backend_ops_s {
  int  (*init)(turbo_datagram_t *d, const char *host, unsigned short port);
  int  (*connect)(turbo_datagram_t *d, const char *host, unsigned short port);
  int  (*send_buffer)(turbo_datagram_t *d, const struct sockaddr *dest,
                      mem_buffer_t *buf, size_t len);
  int  (*recv_start)(turbo_datagram_t *d);
  void (*recv_stop)(turbo_datagram_t *d);
  void (*close)(turbo_datagram_t *d);
  int  (*get_local_addr)(turbo_datagram_t *d, struct sockaddr_storage *addr);
  int  (*join_multicast)(turbo_datagram_t *d, const char *group, const char *iface);
  int  (*leave_multicast)(turbo_datagram_t *d, const char *group, const char *iface);
  int  (*set_multicast_loop)(turbo_datagram_t *d, int on);
  int  (*set_multicast_ttl)(turbo_datagram_t *d, int ttl);
  int  (*set_broadcast)(turbo_datagram_t *d, int on);
} turbo_datagram_backend_ops_t;

/* ── Datagram struct ──────────────────────────────────────── */

struct turbo_datagram_s {
  coro_context_t *ctx;
  turbo_datagram_kind_t kind;
  const turbo_datagram_backend_ops_t *ops;
  mem_pool_t *arena;

  /* Ping-pong recv buffers */
  mem_buffer_t *recv_buf[2];
  int recv_toggle;

  /* Callbacks */
  turbo_recv_cb on_recv;

  /* State */
  int connected;
  int closing;
  int destroyed;      /**< 1 = user called turbo_datagram_destroy() */
  void *user_data;
  void *backend_data; /**< Backend-private state */
};

/* ── Backend selection ────────────────────────────────────── */

const turbo_datagram_backend_ops_t *turbo_datagram_resolve_backend(
    coro_context_t *ctx, turbo_datagram_kind_t kind);

/* ── Per-backend entry points ─────────────────────────────── */

extern const turbo_datagram_backend_ops_t turbo_datagram_iocp_ops;
extern const turbo_datagram_backend_ops_t turbo_datagram_io_uring_ops;
extern const turbo_datagram_backend_ops_t turbo_datagram_epoll_ops;
extern const turbo_datagram_backend_ops_t turbo_datagram_kqueue_ops;

/* ── Shared helpers ───────────────────────────────────────── */

int turbo_datagram_init_common(turbo_datagram_t *d, coro_context_t *ctx,
                                turbo_datagram_kind_t kind,
                                const turbo_datagram_backend_ops_t *ops);

void turbo_datagram_finalize_close(turbo_datagram_t *d);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_DATAGRAM_INTERNAL_H */
