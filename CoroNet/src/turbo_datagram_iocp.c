/**
 * @file turbo_datagram_iocp.c
 * @brief Windows IOCP backend for turbo_datagram_t.
 *
 * Migrated from turbo_udp_iocp.c. Completion delivery stays on the loop
 * thread via coro_post.
 */

#include "turbo_datagram_internal.h"
#include "CoroNet/turbo_coro_internal.h"
#include "CoroNet/turbo_coro_context.h"
#include "turbo_buffer.h"
#include "turbo_thread.h"
#include "ring_buffer_spsc.h"
#include "tlog.h"
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <stdlib.h>
#include <string.h>

/* ── IOCP operation types ─────────────────────────────────── */

typedef enum {
  DG_IOCP_OP_SEND = 1,
  DG_IOCP_OP_RECV = 2
} dg_iocp_op_kind_t;

typedef struct dg_iocp_op_s {
  OVERLAPPED overlapped;
  dg_iocp_op_kind_t kind;
  turbo_datagram_t *dg;
  struct dg_iocp_op_s *next;
  mem_buffer_t *buffer;
  size_t length;
  int status;
  DWORD bytes_transferred;
  DWORD flags;
  WSABUF wsabuf;
  struct sockaddr_storage addr;
  int addr_len;
} dg_iocp_op_t;

typedef struct dg_iocp_state_s {
  turbo_datagram_t *dg;
  coro_context_t *ctx;
  volatile LONG stopping;
  HANDLE completion_port;
  HANDLE worker_thread;
  SOCKET socket;
  volatile LONG inflight_count;
  volatile LONG active_ticks;
  volatile LONG shutdown_posted;
  volatile LONG shutdown_started;
  volatile LONG close_finalized;
  ring_spsc_t queue;
  uint8_t *queue_data;
  int recv_started;
} dg_iocp_state_t;

/* ── Queue helpers ────────────────────────────────────────── */

static void dg_queue_push(dg_iocp_state_t *st, dg_iocp_op_t *op) {
  uint8_t *slot;

  for (;;) {
    slot = ring_spsc_write_acquire(&st->queue, sizeof(void *));
    if (slot) {
      memcpy(slot, &op, sizeof(void *));
      ring_spsc_write_release(&st->queue, sizeof(void *));
      return;
    }

    turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
    turbo_thread_yield();
  }
}

static dg_iocp_op_t *dg_queue_pop_all(dg_iocp_state_t *st) {
  size_t available = 0;
  uint8_t *data = ring_spsc_read_acquire(&st->queue, &available);
  if (!data) return NULL;

  dg_iocp_op_t *head = NULL, *tail = NULL;
  size_t count = available / sizeof(void *);

  for (size_t i = 0; i < count; i++) {
    dg_iocp_op_t *op;
    memcpy(&op, data + (i * sizeof(void *)), sizeof(void *));
    op->next = NULL;
    if (tail) {
      tail->next = op;
    } else {
      head = op;
    }
    tail = op;
  }

  ring_spsc_read_release(&st->queue, count * sizeof(void *));
  return head;
}

/* ── Forward declarations ─────────────────────────────────── */

static void dg_iocp_tick(void *arg1, void *arg2);
static int dg_iocp_submit_recv(turbo_datagram_t *d);
static void dg_iocp_shutdown_task(void *arg1, void *arg2);
static void dg_iocp_finish_close(dg_iocp_state_t *st, turbo_datagram_t *d);

static int dg_iocp_has_pending(dg_iocp_state_t *st) {
  if (!st) {
    return 0;
  }

  return InterlockedCompareExchange(&st->inflight_count, 0, 0) > 0 ||
         InterlockedCompareExchange(&st->active_ticks, 0, 0) > 0;
}

static int dg_iocp_schedule_shutdown(dg_iocp_state_t *st, turbo_datagram_t *d) {
  int rc;

  if (!st || !d) {
    return TURBO_EINVAL;
  }

  if (InterlockedExchange(&st->shutdown_posted, 1) != 0) {
    return 0;
  }

  rc = coro_post(st->ctx, dg_iocp_shutdown_task, st, d);
  if (rc != 0) {
    InterlockedExchange(&st->shutdown_posted, 0);
  }

  return rc;
}

static void dg_iocp_shutdown_now(dg_iocp_state_t *st, turbo_datagram_t *d) {
  if (!st || !d) {
    return;
  }

  if (InterlockedCompareExchange(&st->shutdown_started, 1, 0) != 0) {
    return;
  }

  TLOG_DEBUG("IOCP shutdown: completing");
  if (st->worker_thread) {
    PostQueuedCompletionStatus(st->completion_port, 0, 0, NULL);
    WaitForSingleObject(st->worker_thread, 1000);
    CloseHandle(st->worker_thread);
    st->worker_thread = NULL;
  }

  if (st->completion_port) {
    CloseHandle(st->completion_port);
    st->completion_port = NULL;
  }

  coro_context_release_external(st->ctx);
  dg_iocp_finish_close(st, d);
}

static void dg_iocp_maybe_shutdown(dg_iocp_state_t *st, turbo_datagram_t *d) {
  if (!st || !d || InterlockedCompareExchange(&st->stopping, 0, 0) == 0) {
    return;
  }

  if (dg_iocp_has_pending(st)) {
    (void)dg_iocp_schedule_shutdown(st, d);
    return;
  }

  if (InterlockedCompareExchange(&st->shutdown_posted, 0, 0) != 0) {
    return;
  }

  dg_iocp_shutdown_now(st, d);
}

/* ── IOCP worker thread ──────────────────────────────────── */

static DWORD WINAPI dg_iocp_worker(LPVOID arg) {
  dg_iocp_state_t *st = (dg_iocp_state_t *)arg;
  DWORD bytes;
  ULONG_PTR key;
  OVERLAPPED *ov;
  int rc;

  while (1) {
    BOOL ok = GetQueuedCompletionStatus(st->completion_port, &bytes, &key,
                                         &ov, INFINITE);
    if (!ov) {
      break;
    }
    dg_iocp_op_t *op = CONTAINING_RECORD(ov, dg_iocp_op_t, overlapped);
    op->bytes_transferred = bytes;
    op->status = ok ? 0 : -(int)GetLastError();
    dg_queue_push(st, op);
    InterlockedIncrement(&st->active_ticks);
    for (;;) {
      rc = coro_post(st->ctx, dg_iocp_tick, st, NULL);
      if (rc == 0) {
        break;
      }
      turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
      turbo_thread_yield();
    }
  }
  return 0;
}

/* ── Tick: process completions on loop thread ─────────────── */

static void dg_iocp_tick(void *arg1, void *arg2) {
  dg_iocp_state_t *st = (dg_iocp_state_t *)arg1;
  (void)arg2;

  dg_iocp_op_t *chain = dg_queue_pop_all(st);
  while (chain) {
    dg_iocp_op_t *next = chain->next;
    turbo_datagram_t *d = chain->dg;

    if (chain->kind == DG_IOCP_OP_RECV) {
      if (chain->status == 0 && chain->bytes_transferred > 0) {
        mem_buffer_t *buf = d->recv_buf[d->recv_toggle];
        mem_slice_t slice;
        d->status = 0;
        slice.data = buf->data;
        slice.length = chain->bytes_transferred;
        slice.buffer = buf;
        mem_ref(buf);
        d->recv_toggle ^= 1;

        if (d->on_recv) {
          d->on_recv(d, &slice, (void *)&chain->addr);
          mem_slice_release(&slice);
        } else {
          mem_slice_release(&slice);
        }

        /* Re-arm recv */
        if (st->recv_started && !d->closing) {
          dg_iocp_submit_recv(d);
        }
      } else {
        d->status = (chain->status != 0) ? chain->status : TURBO_EOF;
        if (d->on_recv) d->on_recv(d, NULL, NULL);
      }
    } else if (chain->kind == DG_IOCP_OP_SEND) {
      if (chain->buffer) mem_unref(chain->buffer);
    }
    InterlockedDecrement(&st->inflight_count);
    free(chain);
    chain = next;
  }
  InterlockedDecrement(&st->active_ticks);
  dg_iocp_maybe_shutdown(st, st->dg);
}

static void dg_iocp_finish_close(dg_iocp_state_t *st, turbo_datagram_t *d) {
  if (!st || !d) return;
  if (InterlockedCompareExchange(&st->close_finalized, 1, 0) != 0) return;

  d->backend_data = NULL;
  if (st->queue_data) free(st->queue_data);
  free(st);
  turbo_datagram_finalize_close(d);
}

/* ── Submit recv ──────────────────────────────────────────── */

static int dg_iocp_submit_recv(turbo_datagram_t *d) {
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;

  dg_iocp_op_t *op = (dg_iocp_op_t *)calloc(1, sizeof(dg_iocp_op_t));
  if (!op) return TURBO_ENOMEM;

  op->kind = DG_IOCP_OP_RECV;
  op->dg = d;

  mem_buffer_t *buf = d->recv_buf[d->recv_toggle];
  op->wsabuf.buf = buf->data;
  op->wsabuf.len = (ULONG)buf->capacity;

  op->flags = 0;
  op->addr_len = sizeof(struct sockaddr_storage);
  InterlockedIncrement(&st->inflight_count);
  int rc = WSARecvFrom(st->socket, &op->wsabuf, 1, NULL, &op->flags,
                        (struct sockaddr *)&op->addr, &op->addr_len,
                        &op->overlapped, NULL);
  if (rc == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err != WSA_IO_PENDING) {
      InterlockedDecrement(&st->inflight_count);
      free(op);
      return -(int)err;
    }
  }
  return 0;
}

/* ── Backend ops ──────────────────────────────────────────── */

static int dg_iocp_init(turbo_datagram_t *d, const char *host,
                          unsigned short port) {
  dg_iocp_state_t *st = (dg_iocp_state_t *)calloc(1, sizeof(dg_iocp_state_t));
  if (!st) return TURBO_ENOMEM;

  st->dg = d;
  st->ctx = d->ctx;

  int af = (d->kind == TURBO_DATAGRAM_UDP6) ? AF_INET6 : AF_INET;
  st->socket = WSASocketW(af, SOCK_DGRAM, IPPROTO_UDP, NULL, 0,
                            WSA_FLAG_OVERLAPPED);
  if (st->socket == INVALID_SOCKET) {
    free(st);
    return -(int)WSAGetLastError();
  }

  /* Queue size: 1024 pointers (8KB) */
  st->queue_data = (uint8_t *)calloc(1024, sizeof(void *));
  if (!st->queue_data) {
    closesocket(st->socket);
    free(st);
    return TURBO_ENOMEM;
  }
  ring_spsc_init(&st->queue, st->queue_data, 1024 * sizeof(void *));

  /* Allow address reuse (required for multicast/mDNS) */
  int reuse = 1;
  setsockopt(st->socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

  st->completion_port = CreateIoCompletionPort((HANDLE)st->socket, NULL, 0, 1);
  if (!st->completion_port) {
    closesocket(st->socket);
    if (st->queue_data) free(st->queue_data);
    free(st);
    return TURBO_ENOMEM;
  }

  /* Bind */
  if (af == AF_INET) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = (host && host[0]) ? inet_addr(host) : INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(st->socket, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
      int err = -(int)WSAGetLastError();
      closesocket(st->socket);
      CloseHandle(st->completion_port);
      if (st->queue_data) free(st->queue_data);
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
      CloseHandle(st->completion_port);
      if (st->queue_data) free(st->queue_data);
      free(st);
      return err;
    }
  }

  coro_context_acquire_external(st->ctx);
  st->worker_thread = CreateThread(NULL, 0, dg_iocp_worker, st, 0, NULL);
  if (!st->worker_thread) {
    coro_context_release_external(st->ctx);
    closesocket(st->socket);
    CloseHandle(st->completion_port);
    if (st->queue_data) free(st->queue_data);
    free(st);
    return TURBO_ENOMEM;
  }

  d->backend_data = st;
  return 0;
}

static int dg_iocp_connect(turbo_datagram_t *d, const char *host,
                             unsigned short port) {
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

  dg_iocp_op_t *op = (dg_iocp_op_t *)calloc(1, sizeof(dg_iocp_op_t));
  if (!op) return TURBO_ENOMEM;

  op->kind = DG_IOCP_OP_SEND;
  op->dg = d;
  op->buffer = buf;
  op->length = len;
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
  int rc = WSASendTo(st->socket, &op->wsabuf, 1, NULL, 0, dest, dest_len,
                      &op->overlapped, NULL);
  if (rc == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err != WSA_IO_PENDING) {
      InterlockedDecrement(&st->inflight_count);
      mem_unref(buf);
      free(op);
      return -(int)err;
    }
  }
  return 0;
}

static int dg_iocp_recv_start(turbo_datagram_t *d) {
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  if (!st) return TURBO_EINVAL;
  st->recv_started = 1;
  return dg_iocp_submit_recv(d);
}

static void dg_iocp_recv_stop(turbo_datagram_t *d) {
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  if (st) st->recv_started = 0;
}

static void dg_iocp_shutdown_task(void *arg1, void *arg2) {
  dg_iocp_state_t *st = (dg_iocp_state_t *)arg1;
  turbo_datagram_t *d = (turbo_datagram_t *)arg2;

  if (!st || !d) return;
  InterlockedExchange(&st->shutdown_posted, 0);
  dg_iocp_maybe_shutdown(st, d);
}

static void dg_iocp_close(turbo_datagram_t *d) {
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  if (!st) { turbo_datagram_finalize_close(d); return; }

  InterlockedExchange(&st->stopping, 1);

  if (st->socket != INVALID_SOCKET) {
    closesocket(st->socket);
    st->socket = INVALID_SOCKET;
  }
  dg_iocp_maybe_shutdown(st, d);
}

static int dg_iocp_get_local_addr(turbo_datagram_t *d,
                                    struct sockaddr_storage *addr) {
  if (!d || !d->backend_data || !addr) return TURBO_EINVAL;
  dg_iocp_state_t *st = (dg_iocp_state_t *)d->backend_data;
  int len = (int)sizeof(struct sockaddr_storage);
  if (getsockname(st->socket, (struct sockaddr *)addr, &len) == SOCKET_ERROR)
    return -(int)WSAGetLastError();
  return 0;
}

static int dg_iocp_join_multicast(turbo_datagram_t *d, const char *group,
                                    const char *iface) {
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

static int dg_iocp_leave_multicast(turbo_datagram_t *d, const char *group,
                                     const char *iface) {
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
};
