/**
 * @file turbo_datagram_iocp.c
 * @brief Windows datagram backend using the shared IOCP pool.
 *
 * All datagram operations are registered to the context's shared iocp_pool.
 */

#ifdef _WIN32

#include "turbo_datagram_internal.h"
#include "turbo_iocp_pool.h"
#include "turbo_coro_internal.h"
#include "turbo_thread.h"
#include "tlog.h"
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

typedef struct dg_iocp_state_s {
  SOCKET socket;
  volatile LONG inflight_count;
  int recv_started;
  int closing;
  volatile LONG shutdown_complete;
  iocp_op_t recv_op;
} dg_iocp_state_t;

/* ── Forward declarations ─────────────────────────────────── */

static int dg_iocp_submit_recv(turbo_datagram_t *d);

/* ── Helpers ──────────────────────────────────────────────── */

static void dg_maybe_shutdown(turbo_datagram_t *d) {
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  if (!st || !st->closing) return;
  if (InterlockedCompareExchange(&st->inflight_count, 0, 0) == 0) {
    if (InterlockedCompareExchange(&st->shutdown_complete, 1, 0) != 0) {
      return;
    }
    if (st->socket != INVALID_SOCKET) {
      closesocket(st->socket);
      st->socket = INVALID_SOCKET;
    }
    turbo_datagram_finalize_close(d);
  }
}

/* ── Completion Handlers ──────────────────────────────────── */

void datagram_iocp_handle_recv_op(iocp_op_t *op) {
  turbo_datagram_t *d = (turbo_datagram_t *)op->owner;
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  int status = op->status;
  DWORD bytes = op->bytes_transferred;

  InterlockedDecrement(&st->inflight_count);
  iocp_pool_inflight_dec(d->ctx->iocp_pool);

  if (st->closing) {
    /* no free(op), uses st->recv_op */
    dg_maybe_shutdown(d);
    turbo_datagram_release(d);
    return;
  }

  if (status == 0 && bytes > 0) {
    mem_buffer_t *buf = d->recv_buf[d->recv_toggle];
    mem_slice_t slice;
    d->status = 0;
    slice.data = buf->data;
    slice.length = bytes;
    slice.buffer = buf;
    mem_ref(buf);
    d->recv_toggle ^= 1;

    if (d->on_recv) {
      d->on_recv(d, &slice, (void *)&op->addr);
      mem_slice_release(&slice);
    } else {
      mem_slice_release(&slice);
    }

    if (st->recv_started && !st->closing) {
      dg_iocp_submit_recv(d);
    }
  } else {
    d->status = (status != 0) ? status : TURBO_EOF;
    if (d->on_recv) d->on_recv(d, NULL, NULL);
  }
  /* no free(op), uses st->recv_op */
  turbo_datagram_release(d);
}

void datagram_iocp_handle_send_op(iocp_op_t *op) {
  turbo_datagram_t *d = (turbo_datagram_t *)op->owner;
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;

  if (op->buffer && op->owns_buffer) {
    mem_unref(op->buffer);
  }

  InterlockedDecrement(&st->inflight_count);
  iocp_pool_inflight_dec(d->ctx->iocp_pool);
  free(op);

  if (st->closing) {
    dg_maybe_shutdown(d);
  }
  turbo_datagram_release(d);
}

/* ── Submit Helpers ───────────────────────────────────────── */

static int dg_iocp_submit_recv(turbo_datagram_t *d) {
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;

  iocp_op_t *op = &st->recv_op;
  memset(&op->overlapped, 0, sizeof(OVERLAPPED));

  op->kind = IOCP_OP_DG_RECV;
  op->owner = d;
  turbo_datagram_retain(d);

  mem_buffer_t *buf = d->recv_buf[d->recv_toggle];
  op->wsabuf.buf = buf->data;
  op->wsabuf.len = (ULONG)buf->capacity;
  op->flags = 0;
  op->addr_len = sizeof(struct sockaddr_storage);

  InterlockedIncrement(&st->inflight_count);
  iocp_pool_inflight_inc(d->ctx->iocp_pool);

  int rc = WSARecvFrom(st->socket, &op->wsabuf, 1, NULL, &op->flags,
                       (struct sockaddr *)&op->addr, &op->addr_len,
                       &op->overlapped, NULL);
  if (rc == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err != WSA_IO_PENDING) {
      InterlockedDecrement(&st->inflight_count);
      iocp_pool_inflight_dec(d->ctx->iocp_pool);
      turbo_datagram_release(d);
      return -(int)err;
    }
  }
  return 0;
}

/* ── Backend Ops ──────────────────────────────────────────── */

static int lazy_pool_init(coro_context_t *ctx) {
  if (!ctx->iocp_pool) {
    ctx->iocp_pool = iocp_pool_create(ctx, 0); 
    if (!ctx->iocp_pool) return TURBO_ENOMEM;
  }
  return 0;
}

static int dg_iocp_init(turbo_datagram_t *d, const char *host, unsigned short port) {
  if (lazy_pool_init(d->ctx) != 0) return TURBO_ENOMEM;

  dg_iocp_state_t *st = (dg_iocp_state_t *)calloc(1, sizeof(dg_iocp_state_t));
  if (!st) return TURBO_ENOMEM;

  int af = (d->kind == TURBO_DATAGRAM_UDP6) ? AF_INET6 : AF_INET;
  st->socket = WSASocketW(af, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
  if (st->socket == INVALID_SOCKET) {
    free(st);
    return -(int)WSAGetLastError();
  }

  int reuse = 1;
  setsockopt(st->socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

  if (iocp_pool_associate(d->ctx->iocp_pool, st->socket) != 0) {
    closesocket(st->socket);
    free(st);
    return -(int)GetLastError();
  }

  if (af == AF_INET) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = (host && host[0]) ? inet_addr(host) : INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(st->socket, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
      int err = -(int)WSAGetLastError();
      closesocket(st->socket);
      free(st);
      return err;
    }
  } else {
    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(port);
    if (host && host[0]) inet_pton(AF_INET6, host, &addr6.sin6_addr);
    if (bind(st->socket, (struct sockaddr *)&addr6, sizeof(addr6)) == SOCKET_ERROR) {
      int err = -(int)WSAGetLastError();
      closesocket(st->socket);
      free(st);
      return err;
    }
  }

  d->backend_data = st;
  return 0;
}

static int dg_iocp_connect(turbo_datagram_t *d, const char *host, unsigned short port) {
  if (!d || !d->backend_data) return TURBO_EINVAL;
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;

  int af = (d->kind == TURBO_DATAGRAM_UDP6) ? AF_INET6 : AF_INET;
  if (af == AF_INET) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = host ? inet_addr(host) : inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    if (connect(st->socket, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
      return -(int)WSAGetLastError();
  } else {
    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(port);
    if (host) inet_pton(AF_INET6, host, &addr6.sin6_addr);
    if (connect(st->socket, (struct sockaddr *)&addr6, sizeof(addr6)) == SOCKET_ERROR)
      return -(int)WSAGetLastError();
  }
  return 0;
}

static int dg_iocp_send_buffer(turbo_datagram_t *d, const struct sockaddr *dest,
                                mem_buffer_t *buf, size_t len) {
  if (!d || !d->backend_data || !buf) return TURBO_EINVAL;
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;

  iocp_op_t *op = (iocp_op_t *)calloc(1, sizeof(iocp_op_t));
  if (!op) return TURBO_ENOMEM;

  op->kind = IOCP_OP_DG_SEND;
  op->owner = d;
  turbo_datagram_retain(d);
  op->buffer = buf;
  op->length = len;
  op->owns_buffer = 1;
  mem_ref(buf);

  op->wsabuf.buf = buf->data;
  op->wsabuf.len = (ULONG)len;

  int dest_len = 0;
  if (dest) {
    dest_len = (dest->sa_family == AF_INET6)
      ? (int)sizeof(struct sockaddr_in6)
      : (int)sizeof(struct sockaddr_in);
  }

  InterlockedIncrement(&st->inflight_count);
  iocp_pool_inflight_inc(d->ctx->iocp_pool);

  int rc = WSASendTo(st->socket, &op->wsabuf, 1, NULL, 0, dest, dest_len,
                     &op->overlapped, NULL);
  if (rc == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err != WSA_IO_PENDING) {
      InterlockedDecrement(&st->inflight_count);
      iocp_pool_inflight_dec(d->ctx->iocp_pool);
      mem_unref(buf);
      free(op);
      turbo_datagram_release(d);
      return -(int)err;
    }
  }
  return 0;
}

static int dg_iocp_recv_start(turbo_datagram_t *d) {
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  if (!st) return TURBO_EINVAL;
  if (st->recv_started) return 0;
  st->recv_started = 1;
  return dg_iocp_submit_recv(d);
}

static void dg_iocp_recv_stop(turbo_datagram_t *d) {
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  if (st) st->recv_started = 0;
}

static void dg_iocp_close(turbo_datagram_t *d) {
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  if (!st) { turbo_datagram_finalize_close(d); return; }

  st->closing = 1;
  if (st->socket != INVALID_SOCKET) {
    closesocket(st->socket);
    st->socket = INVALID_SOCKET;
  }
  dg_maybe_shutdown(d);
}

static void dg_iocp_destroy_backend(turbo_datagram_t *d) {
  dg_iocp_state_t *st;

  if (!d) return;

  st = (dg_iocp_state_t *)d->backend_data;
  d->backend_data = NULL;
  if (st) {
    free(st);
  }
}

static int dg_iocp_get_local_addr(turbo_datagram_t *d, struct sockaddr_storage *addr) {
  if (!d || !d->backend_data || !addr) return TURBO_EINVAL;
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  int len = (int)sizeof(struct sockaddr_storage);
  if (getsockname(st->socket, (struct sockaddr *)addr, &len) == SOCKET_ERROR)
    return -(int)WSAGetLastError();
  return 0;
}

static int dg_iocp_join_multicast(turbo_datagram_t *d, const char *group, const char *iface) {
  if (!d || !d->backend_data || !group) return TURBO_EINVAL;
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  struct ip_mreq mreq;
  memset(&mreq, 0, sizeof(mreq));
  mreq.imr_multiaddr.s_addr = inet_addr(group);
  mreq.imr_interface.s_addr = (iface && iface[0]) ? inet_addr(iface) : INADDR_ANY;
  if (setsockopt(st->socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                 (const char *)&mreq, sizeof(mreq)) == SOCKET_ERROR)
    return -(int)WSAGetLastError();
  return 0;
}

static int dg_iocp_leave_multicast(turbo_datagram_t *d, const char *group, const char *iface) {
  if (!d || !d->backend_data || !group) return TURBO_EINVAL;
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  struct ip_mreq mreq;
  memset(&mreq, 0, sizeof(mreq));
  mreq.imr_multiaddr.s_addr = inet_addr(group);
  mreq.imr_interface.s_addr = (iface && iface[0]) ? inet_addr(iface) : INADDR_ANY;
  if (setsockopt(st->socket, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                 (const char *)&mreq, sizeof(mreq)) == SOCKET_ERROR)
    return -(int)WSAGetLastError();
  return 0;
}

static int dg_iocp_set_multicast_loop(turbo_datagram_t *d, int on) {
  if (!d || !d->backend_data) return TURBO_EINVAL;
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  DWORD val = on ? 1 : 0;
  if (setsockopt(st->socket, IPPROTO_IP, IP_MULTICAST_LOOP,
                 (const char *)&val, sizeof(val)) == SOCKET_ERROR)
    return -(int)WSAGetLastError();
  return 0;
}

static int dg_iocp_set_multicast_ttl(turbo_datagram_t *d, int ttl) {
  if (!d || !d->backend_data) return TURBO_EINVAL;
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  DWORD val = (DWORD)ttl;
  if (setsockopt(st->socket, IPPROTO_IP, IP_MULTICAST_TTL,
                 (const char *)&val, sizeof(val)) == SOCKET_ERROR)
    return -(int)WSAGetLastError();
  return 0;
}

static int dg_iocp_set_broadcast(turbo_datagram_t *d, int on) {
  if (!d || !d->backend_data) return TURBO_EINVAL;
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  BOOL val = on ? TRUE : FALSE;
  if (setsockopt(st->socket, SOL_SOCKET, SO_BROADCAST,
                 (const char *)&val, sizeof(val)) == SOCKET_ERROR)
    return -(int)WSAGetLastError();
  return 0;
}

const turbo_datagram_backend_ops_t turbo_datagram_iocp_ops = {
  .init            = dg_iocp_init,
  .connect         = dg_iocp_connect,
  .send_buffer     = dg_iocp_send_buffer,
  .recv_start      = dg_iocp_recv_start,
  .recv_stop       = dg_iocp_recv_stop,
  .close           = dg_iocp_close,
  .get_local_addr  = dg_iocp_get_local_addr,
  .join_multicast  = dg_iocp_join_multicast,
  .leave_multicast = dg_iocp_leave_multicast,
  .set_multicast_loop = dg_iocp_set_multicast_loop,
  .set_multicast_ttl  = dg_iocp_set_multicast_ttl,
  .set_broadcast   = dg_iocp_set_broadcast,
  .destroy_backend = dg_iocp_destroy_backend,
};

#endif /* _WIN32 */
