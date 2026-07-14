/**
 * @file turbo_stream_iocp.c
 * @brief Windows stream (TCP) backend using the shared IOCP pool.
 *
 * All stream operations are registered to the context's shared iocp_pool.
 * Completions are delivered on the loop thread via iocp_pool_drain().
 */

#ifdef _WIN32

#include "turbo_stream_internal.h"
#include "turbo_iocp_pool.h"
#include "turbo_coro_internal.h"
#include "turbo_thread.h"
#include "tlog.h"
#include <mswsock.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#define STREAM_IOCP_ACCEPT_DEPTH 8
/* Completion callbacks are serialized by the owning event loop. A single
 * dequeue worker preserves completion order without reducing callback
 * parallelism. */
#define STREAM_IOCP_COMPLETION_WORKERS 1

void turbo_stream_tls_note_iocp_timing(turbo_stream_t *s, uint64_t iocp_post_ns,
                                       uint64_t post_drain_ns);

/* ── Per-stream IOCP state ────────────────────────────────── */

typedef struct stream_iocp_state_s {
  SOCKET socket;
  int socket_bound;
  LPFN_CONNECTEX connect_ex;
  volatile LONG inflight_count;
  int recv_started;
  int recv_inflight;
  int closing;
  int connect_pending;
  int connect_reported;

  /* Dedicated ops for zero-alloc datapaths */
  iocp_op_t recv_op;

  iocp_op_t send_op;
  int send_inflight;
  WSABUF send_wsabuf;
  struct mem_buffer_s *send_buffer;
} stream_iocp_state_t;

typedef struct stream_iocp_server_state_s {
  SOCKET socket;
  int accept_family;
  int accept_depth;
  LPFN_ACCEPTEX accept_ex;
  LPFN_GETACCEPTEXSOCKADDRS get_accept_ex_sockaddrs;
  volatile LONG accepts_posted;
  volatile LONG inflight_count;
  int closing;
  CRITICAL_SECTION shutdown_lock;
} stream_iocp_server_state_t;

/* ── Forward declarations ─────────────────────────────────── */

static int stream_iocp_submit_recv(turbo_stream_t *s);
static int stream_iocp_submit_send(turbo_stream_t *s);
static int stream_iocp_submit_accept(turbo_stream_listener_t *l);
static int iocp_init_with_socket(turbo_stream_t *s, SOCKET existing);



static int stream_iocp_send_sync_buffer(turbo_stream_t *s,
                                        stream_iocp_state_t *st,
                                        mem_buffer_t *buf) {
  size_t total = 0;

  while (total < buf->used) {
    int rc = send(st->socket, buf->data + total, (int)(buf->used - total), 0);
    if (rc == SOCKET_ERROR) {
      return -(int)WSAGetLastError();
    }
    if (rc == 0) {
      return TURBO_EOF;
    }
    total += (size_t)rc;
  }


  return 0;
}

/* ── Helpers ──────────────────────────────────────────────── */

static void stream_report_connect_once(turbo_stream_t *s, stream_iocp_state_t *st, int status) {
  if (!s || !st || st->connect_reported) return;

  st->connect_reported = 1;
  st->connect_pending = 0;
  if (s->on_connect) {
    turbo_stream_callback_enter(s);
    s->on_connect(s, status, NULL);
    turbo_stream_callback_leave(s);
    turbo_stream_maybe_free(s);
  }
}

static void stream_maybe_shutdown(turbo_stream_t *s) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  if (!st || !st->closing) return;
  if (InterlockedCompareExchange(&st->inflight_count, 0, 0) == 0) {
    if (st->socket != INVALID_SOCKET) {
      closesocket(st->socket);
      st->socket = INVALID_SOCKET;
    }
    free(st);
    s->backend_data = NULL;
    turbo_stream_finalize_close(s);
  }
}

static int listener_can_shutdown_locked(turbo_stream_listener_t *l,
                                        stream_iocp_server_state_t *st) {
  if (!l || !st || !st->closing) return 0;
  if (l->active_connections > 0) return 0;
  return InterlockedCompareExchange(&st->inflight_count, 0, 0) == 0;
}

static void listener_finalize_shutdown(turbo_stream_listener_t *l,
                                       stream_iocp_server_state_t *st) {
  if (!l || !st) return;

  EnterCriticalSection(&st->shutdown_lock);
  if (!listener_can_shutdown_locked(l, st)) {
    LeaveCriticalSection(&st->shutdown_lock);
    return;
  }
  if (st->socket != INVALID_SOCKET) {
    closesocket(st->socket);
    st->socket = INVALID_SOCKET;
  }
  l->backend_data = NULL;
  LeaveCriticalSection(&st->shutdown_lock);

  DeleteCriticalSection(&st->shutdown_lock);
  free(st);
  turbo_stream_listener_finalize_close(l);
}

static void listener_maybe_shutdown(turbo_stream_listener_t *l) {
  stream_iocp_server_state_t *st = (stream_iocp_server_state_t *)l->backend_data;
  if (!st) return;
  listener_finalize_shutdown(l, st);
}

void turbo_stream_iocp_listener_on_connection_closed(turbo_stream_listener_t *l) {
  if (!l) {
    return;
  }

  listener_maybe_shutdown(l);
}

/* ── Completion handlers ──────────────────────────────────── */

void stream_iocp_handle_connect_op(iocp_op_t *op) {
  turbo_stream_t *s = (turbo_stream_t *)op->owner;
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  int status = op->status;
  uint64_t handler_entry_ns = turbo_hrtime();

  if (op->completed_ns != 0) {
    uint64_t iocp_post_ns = 0;
    uint64_t post_drain_ns;

    if (op->tick_post_request_ns != 0 && op->tick_post_request_ns >= op->completed_ns) {
      iocp_post_ns = op->tick_post_request_ns - op->completed_ns;
      post_drain_ns = handler_entry_ns - op->tick_post_request_ns;
    } else {
      post_drain_ns = handler_entry_ns - op->completed_ns;
    }

    turbo_stream_tls_note_iocp_timing(s, iocp_post_ns, post_drain_ns);
  }

  InterlockedDecrement(&st->inflight_count);
  iocp_pool_inflight_dec(s->ctx->iocp_pool); /* context-level */
  free(op);

  if (st->closing) {
    stream_maybe_shutdown(s);
    return;
  }

  if (status == 0) {
    setsockopt(st->socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
    s->connected = 1;
    if (s->on_recv) {
      stream_iocp_submit_recv(s);
    }
  }

  stream_report_connect_once(s, st, status);
}

void stream_iocp_handle_send_op(iocp_op_t *op) {
  turbo_stream_t *s = (turbo_stream_t *)op->owner;
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  int status = op->status;


  /* Clear the state tracking the active send buffer */
  if (st->send_buffer) {
    mem_unref(st->send_buffer);
    st->send_buffer = NULL;
  }
  st->send_inflight = 0;

  InterlockedDecrement(&st->inflight_count);
  iocp_pool_inflight_dec(s->ctx->iocp_pool);
  /* no free(op), it is &st->send_op */

  if (st->closing) {
    stream_maybe_shutdown(s);
    return;
  }

  if (s->on_write_complete) {
    turbo_stream_callback_enter(s);
    s->on_write_complete(s, status);
    turbo_stream_callback_leave(s);
    if (s->finalized) {
      turbo_stream_maybe_free(s);
      return;
    }
  }

  if (s->send_head && !s->closing) {
    if (stream_iocp_submit_send(s) != 0) {
      return;
    }
  }

  turbo_stream_maybe_free(s);
}

void stream_iocp_handle_recv_op(iocp_op_t *op) {
  turbo_stream_t *s = (turbo_stream_t *)op->owner;
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  int status = op->status;
  DWORD bytes = op->bytes_transferred;
  uint64_t handler_entry_ns = turbo_hrtime();

  st->recv_inflight = 0;

  if (op->completed_ns != 0) {
    uint64_t iocp_post_ns = 0;
    uint64_t post_drain_ns;

    if (op->tick_post_request_ns != 0 && op->tick_post_request_ns >= op->completed_ns) {
      iocp_post_ns = op->tick_post_request_ns - op->completed_ns;
      post_drain_ns = handler_entry_ns - op->tick_post_request_ns;
    } else {
      post_drain_ns = handler_entry_ns - op->completed_ns;
    }

    turbo_stream_tls_note_iocp_timing(s, iocp_post_ns, post_drain_ns);
  }

  InterlockedDecrement(&st->inflight_count);
  iocp_pool_inflight_dec(s->ctx->iocp_pool);
  /* no free(op), it is &st->recv_op */

  if (st->closing) {
    stream_maybe_shutdown(s);
    return;
  }

  if (status != 0 || bytes == 0) {
    if (s->on_recv) {
      turbo_stream_callback_enter(s);
      s->on_recv(s, NULL, NULL);
      turbo_stream_callback_leave(s);
      if (s->finalized) {
        turbo_stream_maybe_free(s);
        return;
      }
    }
    turbo_stream_close(s);
    return;
  }

  mem_buffer_t *buf = s->recv_buf[s->recv_toggle];
  mem_slice_t slice;
  slice.data = buf->data;
  slice.length = bytes;
  slice.buffer = buf;
  mem_ref(buf);

  s->recv_toggle ^= 1;

  if (s->on_recv) {
    int close_requested;
    turbo_stream_callback_enter(s);
    close_requested = s->on_recv(s, &slice, NULL);
    turbo_stream_callback_leave(s);
    mem_slice_release(&slice);
    if (s->finalized) {
      turbo_stream_maybe_free(s);
      return;
    }
    if (close_requested) {
      turbo_stream_close(s);
      return;
    }
  } else {
    mem_slice_release(&slice);
  }

  /* on_recv may synchronously close the stream and release backend_data. */
  st = (stream_iocp_state_t *)s->backend_data;
  if (st != NULL && st->recv_started && !s->closing && !st->closing) {
    stream_iocp_submit_recv(s);
  }

  turbo_stream_maybe_free(s);
}

void stream_iocp_handle_accept_op(iocp_op_t *op) {
  turbo_stream_listener_t *l = (turbo_stream_listener_t *)op->owner;
  stream_iocp_server_state_t *st = (stream_iocp_server_state_t *)l->backend_data;
  int status = op->status;
  SOCKET client_socket = op->client_socket;

  if (st->closing) {
    if (client_socket != INVALID_SOCKET) closesocket(client_socket);
    InterlockedDecrement(&st->inflight_count);
    InterlockedDecrement(&st->accepts_posted);
    iocp_pool_inflight_dec(l->ctx->iocp_pool);
    free(op);
    listener_maybe_shutdown(l);
    return;
  }

  if (status == 0) {
    setsockopt(client_socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
               (const char *)&st->socket, sizeof(st->socket));

    struct sockaddr *p_local = NULL, *p_remote = NULL;
    int local_len = 0, remote_len = 0;
    st->get_accept_ex_sockaddrs(op->accept_buf, 0,
                                sizeof(struct sockaddr_storage) + 16,
                                sizeof(struct sockaddr_storage) + 16,
                                &p_local, &local_len, &p_remote, &remote_len);

    turbo_stream_t *s = turbo_stream_create(l->ctx, l->kind);
    if (s) {
      int rc = iocp_init_with_socket(s, client_socket);
      if (rc == 0) rc = turbo_stream_listener_configure_child(l, s);
      if (rc == 0) rc = turbo_stream_apply_native_socket_options(s, client_socket);
      if (rc == 0) {
        s->connected = 1;
        s->listener = l;
        l->active_connections++;
        if (l->on_accept) {
          l->on_accept(l, s, p_remote);
        }
      } else {
        turbo_stream_destroy(s);
      }
    } else {
      closesocket(client_socket);
    }
  } else {
    if (client_socket != INVALID_SOCKET) closesocket(client_socket);
  }

  InterlockedDecrement(&st->inflight_count);
  InterlockedDecrement(&st->accepts_posted);
  iocp_pool_inflight_dec(l->ctx->iocp_pool);
  free(op);

  while (!st->closing && InterlockedCompareExchange(&st->accepts_posted, 0, 0) < st->accept_depth) {
    if (stream_iocp_submit_accept(l) != 0) break;
  }

  listener_maybe_shutdown(l);
}

/* ── Submit helpers ───────────────────────────────────────── */

static int stream_iocp_submit_recv(turbo_stream_t *s) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;

  if (!st || st->recv_inflight || st->closing || s->closing) return 0;

  iocp_op_t *op = &st->recv_op;
  memset(&op->overlapped, 0, sizeof(OVERLAPPED));
  op->kind = IOCP_OP_STREAM_RECV;
  op->owner = s;

  mem_buffer_t *buf = s->recv_buf[s->recv_toggle];
  op->wsabuf.buf = buf->data;
  op->wsabuf.len = (ULONG)buf->capacity;
  op->flags = 0;

  st->recv_inflight = 1;
  InterlockedIncrement(&st->inflight_count);
  iocp_pool_inflight_inc(s->ctx->iocp_pool);
  
  if (WSARecv(st->socket, &op->wsabuf, 1, NULL, &op->flags, &op->overlapped, NULL) == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err != WSA_IO_PENDING) {
      st->recv_inflight = 0;
      InterlockedDecrement(&st->inflight_count);
      iocp_pool_inflight_dec(s->ctx->iocp_pool);
      return -(int)err;
    }
  }
  return 0;
}

static int stream_iocp_submit_send(turbo_stream_t *s) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  if (!st || s->closing || st->closing || st->socket == INVALID_SOCKET) {
    return TURBO_ECANCELED;
  }
  if (!s->send_head) return 0;
  if (st->send_inflight) {
    return 0;
  }

  mem_buffer_t *buf = s->send_head;
  s->send_head = buf->next;
  if (!s->send_head) s->send_tail = NULL;
  s->send_queued -= buf->used;
  buf->next = NULL;

  st->send_buffer = buf;
  st->send_wsabuf.buf = buf->data;
  st->send_wsabuf.len = (ULONG)buf->used;

  if (s->listener != NULL && st->send_wsabuf.len <= 64U) {
    int sync_rc = stream_iocp_send_sync_buffer(s, st, buf);
    st->send_buffer = NULL;
    if (sync_rc != 0) {
      mem_unref(buf);
      turbo_stream_close(s);
      return sync_rc;
    }
    mem_unref(buf);
    if (s->on_write_complete) {
      turbo_stream_callback_enter(s);
      s->on_write_complete(s, 0);
      turbo_stream_callback_leave(s);
      if (s->finalized) {
        turbo_stream_maybe_free(s);
        return 0;
      }
    }
    if (s->send_head && !s->closing) {
      return stream_iocp_submit_send(s);
    }
    turbo_stream_maybe_free(s);
    return 0;
  }

  iocp_op_t *op = &st->send_op;
  memset(&op->overlapped, 0, sizeof(OVERLAPPED));
  op->kind = IOCP_OP_STREAM_SEND;
  op->owner = s;
  op->buffer = NULL;
  op->owns_buffer = 0;
  op->length = 0;

  st->send_inflight = 1;
  InterlockedIncrement(&st->inflight_count);
  iocp_pool_inflight_inc(s->ctx->iocp_pool);

  if (WSASend(st->socket, &st->send_wsabuf, 1, NULL, 0, &op->overlapped, NULL) == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err != WSA_IO_PENDING) {
      TLOG_WARN("iocp_send_submit: failed stream={:p} socket={} len={} err={}",
                (void *)s,
                (unsigned long long)st->socket,
                (unsigned long)st->send_wsabuf.len,
                err);
      InterlockedDecrement(&st->inflight_count);
      iocp_pool_inflight_dec(s->ctx->iocp_pool);
      st->send_inflight = 0;
      mem_unref(st->send_buffer);
      st->send_buffer = NULL;
      turbo_stream_close(s);
      return -(int)err;
    }
  }
  return 0;
}

static int stream_iocp_submit_accept(turbo_stream_listener_t *l) {
  stream_iocp_server_state_t *st = (stream_iocp_server_state_t *)l->backend_data;

  iocp_op_t *op = (iocp_op_t *)calloc(1, sizeof(iocp_op_t));
  if (!op) return TURBO_ENOMEM;

  op->kind = IOCP_OP_STREAM_ACCEPT;
  op->owner = l;
  op->client_socket = WSASocketW(st->accept_family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
  if (op->client_socket == INVALID_SOCKET) {
    free(op);
    return -(int)WSAGetLastError();
  }

  InterlockedIncrement(&st->accepts_posted);
  InterlockedIncrement(&st->inflight_count);
  iocp_pool_inflight_inc(l->ctx->iocp_pool);
  
  DWORD bytes = 0;
  if (!st->accept_ex(st->socket, op->client_socket, op->accept_buf, 0,
                     sizeof(struct sockaddr_storage) + 16,
                     sizeof(struct sockaddr_storage) + 16, &bytes, &op->overlapped)) {
    int err = WSAGetLastError();
    if (err != ERROR_IO_PENDING) {
      InterlockedDecrement(&st->accepts_posted);
      InterlockedDecrement(&st->inflight_count);
      iocp_pool_inflight_dec(l->ctx->iocp_pool);
      closesocket(op->client_socket);
      free(op);
      return -(int)err;
    }
  }
  return 0;
}

/* ── Setup / Initialization ───────────────────────────────── */

static int lazy_pool_init(coro_context_t *ctx) {
  if (!ctx->iocp_pool) {
    ctx->iocp_pool = iocp_pool_create(ctx, STREAM_IOCP_COMPLETION_WORKERS);
    if (!ctx->iocp_pool) return TURBO_ENOMEM;
  }
  return 0;
}

static int iocp_setup_socket(turbo_stream_t *s, stream_iocp_state_t *st, SOCKET sock) {
  if (lazy_pool_init(s->ctx) != 0) return TURBO_ENOMEM;

  if (iocp_pool_associate(s->ctx->iocp_pool, sock) != 0) {
    return -(int)GetLastError();
  }

  GUID guid = WSAID_CONNECTEX;
  DWORD bytes;
  if (WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &st->connect_ex,
               sizeof(st->connect_ex), &bytes, NULL, NULL) == SOCKET_ERROR) {
    return -(int)WSAGetLastError();
  }

  int yes = 1;
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
  {
    int rc = turbo_stream_apply_native_socket_options(s, sock);
    if (rc != 0) return rc;
  }
  st->socket = sock;
  return 0;
}

static int iocp_init_with_socket(turbo_stream_t *s, SOCKET existing) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  if (st) {
    if (existing != INVALID_SOCKET) {
      int rc = iocp_setup_socket(s, st, existing);
      if (rc != 0) {
        closesocket(existing);
        return rc;
      }
      st->socket_bound = 1;
    }
    return 0;
  }

  st = (stream_iocp_state_t *)calloc(1, sizeof(stream_iocp_state_t));
  if (!st) return TURBO_ENOMEM;
  st->socket = INVALID_SOCKET;

  if (existing != INVALID_SOCKET) {
    int rc = iocp_setup_socket(s, st, existing);
    if (rc != 0) {
      closesocket(existing);
      free(st);
      return rc;
    }
    st->socket_bound = 1;
  }

  s->backend_data = st;
  return 0;
}

static int iocp_init(turbo_stream_t *s) { return iocp_init_with_socket(s, INVALID_SOCKET); }

static int iocp_create_socket(turbo_stream_t *s, stream_iocp_state_t *st, int af) {
  if (st->socket != INVALID_SOCKET) return 0;
  SOCKET sock = WSASocketW(af, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
  if (sock == INVALID_SOCKET) return -(int)WSAGetLastError();

  int rc = iocp_setup_socket(s, st, sock);
  if (rc != 0) {
    closesocket(sock);
    return rc;
  }
  return 0;
}

/* ── Public Ops ───────────────────────────────────────────── */

static int iocp_connect(turbo_stream_t *s, const struct sockaddr *addr) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  int af = addr->sa_family;

  int rc = iocp_create_socket(s, st, af);
  if (rc != 0) return rc;

  if (!st->socket_bound) {
    struct sockaddr_storage bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    if (af == AF_INET6) {
      struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&bind_addr;
      a6->sin6_family = AF_INET6;
      a6->sin6_addr = in6addr_any;
      a6->sin6_port = 0;
      rc = bind(st->socket, (struct sockaddr *)a6, sizeof(*a6));
    } else {
      struct sockaddr_in *a4 = (struct sockaddr_in *)&bind_addr;
      a4->sin_family = AF_INET;
      a4->sin_addr.s_addr = INADDR_ANY;
      a4->sin_port = 0;
      rc = bind(st->socket, (struct sockaddr *)a4, sizeof(*a4));
    }
    if (rc == SOCKET_ERROR) return -(int)WSAGetLastError();
    st->socket_bound = 1;
  }

  iocp_op_t *op = (iocp_op_t *)calloc(1, sizeof(iocp_op_t));
  if (!op) return TURBO_ENOMEM;

  op->kind = IOCP_OP_STREAM_CONNECT;
  op->owner = s;
  st->connect_pending = 1;
  st->connect_reported = 0;

  int addr_len = (af == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);

  InterlockedIncrement(&st->inflight_count);
  iocp_pool_inflight_inc(s->ctx->iocp_pool);
  
  if (!st->connect_ex(st->socket, addr, addr_len, NULL, 0, NULL, &op->overlapped)) {
    int err = WSAGetLastError();
    if (err != ERROR_IO_PENDING) {
      InterlockedDecrement(&st->inflight_count);
      iocp_pool_inflight_dec(s->ctx->iocp_pool);
      free(op);
      return -(int)err;
    }
  }

  return 0;
}

static int iocp_connect_pipe(turbo_stream_t *s, const char *name) {
  (void)s; (void)name;
  return TURBO_ENOTSUP; /* Pipe uses pipe_win backend */
}

static int iocp_send(turbo_stream_t *s, const char *data, size_t len) {
  mem_buffer_t *buf = mem_get_buffer(s->arena, len);
  if (!buf) return TURBO_ENOMEM;
  memcpy(buf->data, data, len);
  mem_set_used(buf, len);
  turbo_stream_enqueue_buffer(s, buf);
  mem_unref(buf);
  return stream_iocp_submit_send(s);
}

static int iocp_flush(turbo_stream_t *s) { return stream_iocp_submit_send(s); }

static int iocp_recv_start(turbo_stream_t *s) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  if (st->recv_started) return 0;
  st->recv_started = 1;
  return stream_iocp_submit_recv(s);
}

static void iocp_recv_stop(turbo_stream_t *s) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  st->recv_started = 0;
}

static void iocp_close(turbo_stream_t *s) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  if (!st) {
    turbo_stream_finalize_close(s);
    return;
  }

  if (st->connect_pending && !st->connect_reported) {
    stream_report_connect_once(s, st, TURBO_ECANCELED);
  }

  st->closing = 1;
  /* If WSARecv is pending, closesocket cancels it, invoking completion with error.
     The completion will notice closing=1 and call stream_maybe_shutdown. */
  if (st->socket != INVALID_SOCKET) {
    /* Make pending ConnectEx/WSARecv completions arrive promptly on the IOCP. */
    (void)CancelIoEx((HANDLE)st->socket, NULL);
    closesocket(st->socket);
    st->socket = INVALID_SOCKET;
  }
  stream_maybe_shutdown(s);
}

static int iocp_get_local_addr(turbo_stream_t *s, struct sockaddr_storage *addr) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  if (!st || st->socket == INVALID_SOCKET) return TURBO_EINVAL;
  int len = sizeof(struct sockaddr_storage);
  if (getsockname(st->socket, (struct sockaddr *)addr, &len) == SOCKET_ERROR) {
    return -(int)WSAGetLastError();
  }
  return 0;
}

static int iocp_get_peer_addr(turbo_stream_t *s, struct sockaddr_storage *addr) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  if (!st || st->socket == INVALID_SOCKET) return TURBO_EINVAL;
  int len = sizeof(struct sockaddr_storage);
  if (getpeername(st->socket, (struct sockaddr *)addr, &len) == SOCKET_ERROR) {
    return -(int)WSAGetLastError();
  }
  return 0;
}

/* ── Listener implementation ──────────────────────────────── */

static int iocp_bind_pipe(turbo_stream_listener_t *l, const char *name) {
  (void)l; (void)name;
  return TURBO_ENOTSUP;
}

static int iocp_bind(turbo_stream_listener_t *l, const struct sockaddr *addr) {
  if (!l || !addr) return TURBO_EINVAL;

  if (lazy_pool_init(l->ctx) != 0) return TURBO_ENOMEM;

  SOCKET sock = socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
  if (sock == INVALID_SOCKET) return -(int)WSAGetLastError();

  int on = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
  if (l->reuse_port) {
#ifdef SO_REUSEPORT
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char *)&on, sizeof(on)) == SOCKET_ERROR) {
      int rc = -(int)WSAGetLastError();
      closesocket(sock);
      return rc;
    }
#else
    closesocket(sock);
    return TURBO_ENOTSUP;
#endif
  }

  if (bind(sock, addr, (int)(addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6))) == SOCKET_ERROR) {
    int rc = -(int)WSAGetLastError();
    closesocket(sock);
    return rc;
  }

  if (iocp_pool_associate(l->ctx->iocp_pool, sock) != 0) {
    int rc = -(int)GetLastError();
    closesocket(sock);
    return rc;
  }

  stream_iocp_server_state_t *st = calloc(1, sizeof(stream_iocp_server_state_t));
  if (!st) {
    closesocket(sock);
    return TURBO_ENOMEM;
  }

  st->socket = sock;
  st->accept_family = addr->sa_family;
  st->accept_depth = STREAM_IOCP_ACCEPT_DEPTH;
  InitializeCriticalSection(&st->shutdown_lock);

  GUID accept_guid = WSAID_ACCEPTEX;
  GUID getaddr_guid = WSAID_GETACCEPTEXSOCKADDRS;
  DWORD bytes;
  if (WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &accept_guid, sizeof(accept_guid),
               &st->accept_ex, sizeof(st->accept_ex), &bytes, NULL, NULL) == SOCKET_ERROR) {
    closesocket(sock);
    DeleteCriticalSection(&st->shutdown_lock);
    free(st);
    return -(int)WSAGetLastError();
  }
  if (WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &getaddr_guid, sizeof(getaddr_guid),
               &st->get_accept_ex_sockaddrs, sizeof(st->get_accept_ex_sockaddrs), &bytes, NULL, NULL) == SOCKET_ERROR) {
    closesocket(sock);
    DeleteCriticalSection(&st->shutdown_lock);
    free(st);
    return -(int)WSAGetLastError();
  }

  l->backend_data = st;
  return 0;
}

static int iocp_listen(turbo_stream_listener_t *l, int backlog) {
  stream_iocp_server_state_t *st = (stream_iocp_server_state_t *)l->backend_data;
  if (!st) return TURBO_EINVAL;

  if (listen(st->socket, backlog) == SOCKET_ERROR) {
    return -(int)WSAGetLastError();
  }

  int posted = 0;
  while (posted < st->accept_depth) {
    int rc = stream_iocp_submit_accept(l);
    if (rc != 0) {
      TLOG_ERROR("iocp listener startup failed: unable to post accept {:d}", rc);
      return rc;
    }
    posted++;
  }
  return 0;
}

static void iocp_listener_close(turbo_stream_listener_t *l) {
  stream_iocp_server_state_t *st = (stream_iocp_server_state_t *)l->backend_data;
  if (!st) {
    turbo_stream_listener_finalize_close(l);
    return;
  }
  EnterCriticalSection(&st->shutdown_lock);
  st->closing = 1;
  if (st->socket != INVALID_SOCKET) {
    closesocket(st->socket);
    st->socket = INVALID_SOCKET;
  }
  LeaveCriticalSection(&st->shutdown_lock);
  listener_finalize_shutdown(l, st);
}

const turbo_stream_backend_ops_t turbo_stream_iocp_ops = {
    .init = iocp_init,
    .connect = iocp_connect,
    .connect_pipe = iocp_connect_pipe,
    .send = iocp_send,
    .flush = iocp_flush,
    .recv_start = iocp_recv_start,
    .recv_stop = iocp_recv_stop,
    .close = iocp_close,
    .get_local_addr = iocp_get_local_addr,
    .get_peer_addr = iocp_get_peer_addr,
    .bind = iocp_bind,
    .bind_pipe = iocp_bind_pipe,
    .listen = iocp_listen,
    .listener_close = iocp_listener_close,
};

#endif /* _WIN32 */
