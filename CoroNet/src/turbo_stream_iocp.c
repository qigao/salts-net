/**
 * @file turbo_stream_iocp.c
 * @brief Windows IOCP backend for turbo_stream_t.
 *
 * Migrated from turbo_tcp_iocp.c + turbo_coro_socket_tcp.c IOCP paths.
 * Completion handling stays on the loop thread via coro_post.
 */

#include "turbo_stream_internal.h"
#include "CoroNet/turbo_coro_context.h"
#include "turbo_coro_internal.h"
#include "turbo_buffer.h"
#include "turbo_dns.h"
#include "ring_buffer_spsc.h"

#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <ws2tcpip.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── IOCP operation types ─────────────────────────────────── */

#define STREAM_IOCP_ACCEPT_DEPTH 8

typedef enum {
  STREAM_IOCP_OP_CONNECT = 1,
  STREAM_IOCP_OP_SEND    = 2,
  STREAM_IOCP_OP_RECV    = 3,
  STREAM_IOCP_OP_ACCEPT  = 4
} stream_iocp_op_kind_t;

typedef struct stream_iocp_op_s {
  OVERLAPPED overlapped;          /* must be first for CONTAINING_RECORD */
  stream_iocp_op_kind_t kind;
  void *owner;                    /* turbo_stream_t* or turbo_stream_listener_t* */
  struct stream_iocp_op_s *next;
  mem_buffer_t *buffer;
  int owns_buffer;
  size_t length;
  DWORD bytes_transferred;
  int status;
  DWORD flags;
  SOCKET client_socket;           /* for AcceptEx */
  uint8_t accept_buf[(sizeof(struct sockaddr_storage) + 16) * 2];
} stream_iocp_op_t;

/* ── Per-stream IOCP state ────────────────────────────────── */

typedef struct stream_iocp_base_s {
  void *owner;                    /* turbo_stream_t* or turbo_stream_listener_t* */
  coro_context_t *ctx;
  HANDLE completion_port;
  HANDLE worker_thread;
  SOCKET socket;
  volatile LONG stopping;
  volatile LONG inflight_count;
  volatile LONG active_ticks;
  ring_spsc_t queue;
  uint8_t *queue_data;
} stream_iocp_base_t;

typedef struct stream_iocp_state_s {
  stream_iocp_base_t base;
  int socket_bound;
  LPFN_CONNECTEX connect_ex;
  int recv_started;
  int recv_inflight;
  int send_inflight;
} stream_iocp_state_t;

typedef struct stream_iocp_server_state_s {
  stream_iocp_base_t base;
  LPFN_ACCEPTEX accept_ex;
  LPFN_GETACCEPTEXSOCKADDRS get_accept_ex_sockaddrs;
  int accept_family;
  int accept_depth;
  volatile LONG accepts_posted;
} stream_iocp_server_state_t;

/* ── Forward declarations ─────────────────────────────────── */

static DWORD WINAPI stream_iocp_worker(LPVOID arg);
static void stream_iocp_tick(void *arg1, void *arg2);
static void stream_iocp_handle_connect(stream_iocp_op_t *op);
static void stream_iocp_handle_send(stream_iocp_op_t *op);
static void stream_iocp_handle_recv(stream_iocp_op_t *op);
static void stream_iocp_handle_accept(stream_iocp_op_t *op);
static int stream_iocp_submit_recv(turbo_stream_t *s);
static int stream_iocp_submit_send(turbo_stream_t *s);
static int stream_iocp_submit_accept(turbo_stream_listener_t *l);
static int iocp_init_with_socket(turbo_stream_t *s, SOCKET existing);

/* ── Queue helpers ────────────────────────────────────────── */

static void queue_push(void *st_any, stream_iocp_op_t *op) {
  stream_iocp_base_t *st = (stream_iocp_base_t *)st_any;
  uint8_t *slot = ring_spsc_write_acquire(&st->queue, sizeof(void *));
  if (slot) {
    memcpy(slot, &op, sizeof(void *));
    ring_spsc_write_release(&st->queue, sizeof(void *));
  } else {
    /* Ring buffer full! This shouldn't happen with 1024 slots unless tick is dead.
       Fall back to dropping or something? For now, we just leak or crash to show limit. */
    free(op);
  }
}

static stream_iocp_op_t *queue_pop_all(void *st_any) {
  stream_iocp_base_t *st = (stream_iocp_base_t *)st_any;
  size_t available = 0;
  uint8_t *data = ring_spsc_read_acquire(&st->queue, &available);
  if (!data) return NULL;

  stream_iocp_op_t *head = NULL, *tail = NULL;
  size_t count = available / sizeof(void *);

  for (size_t i = 0; i < count; i++) {
    stream_iocp_op_t *op;
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

/* ── IOCP worker thread ──────────────────────────────────── */

static DWORD WINAPI stream_iocp_worker(LPVOID arg) {
  stream_iocp_base_t *st = (stream_iocp_base_t *)arg;
  DWORD bytes;
  ULONG_PTR key;
  OVERLAPPED *ov;

  while (1) {
    BOOL ok = GetQueuedCompletionStatus(st->completion_port, &bytes, &key,
                                         &ov, INFINITE);
    if (!ov) {
      break;
    }

    stream_iocp_op_t *op = CONTAINING_RECORD(ov, stream_iocp_op_t, overlapped);
    op->bytes_transferred = bytes;
    op->status = ok ? 0 : -(int)GetLastError();

    queue_push(st, op);
    InterlockedIncrement(&st->active_ticks);
    coro_post(st->ctx, stream_iocp_tick, st, NULL);
  }
  return 0;
}

/* ── Tick: process completions on the loop thread ─────────── */

static void stream_iocp_tick(void *arg1, void *arg2) {
  stream_iocp_base_t *st = (stream_iocp_base_t *)arg1;
  (void)arg2;

  stream_iocp_op_t *chain = queue_pop_all(st);
  while (chain) {
    stream_iocp_op_t *next = chain->next;
    switch (chain->kind) {
      case STREAM_IOCP_OP_CONNECT: stream_iocp_handle_connect(chain); break;
      case STREAM_IOCP_OP_SEND:    stream_iocp_handle_send(chain); break;
      case STREAM_IOCP_OP_RECV:    stream_iocp_handle_recv(chain); break;
      case STREAM_IOCP_OP_ACCEPT:  stream_iocp_handle_accept(chain); break;
    }
    InterlockedDecrement(&st->inflight_count);
    chain = next;
  }
  InterlockedDecrement(&st->active_ticks);
}

static void stream_iocp_final_cleanup(void *arg1, void *arg2) {
  stream_iocp_base_t *st = (stream_iocp_base_t *)arg1;
  turbo_stream_t *s = (turbo_stream_t *)arg2;

  if (st->queue_data) free(st->queue_data);
  free(st);
  s->backend_data = NULL;
  turbo_stream_finalize_close(s);
}

/* ── Completion handlers ──────────────────────────────────── */

static void stream_iocp_handle_connect(stream_iocp_op_t *op) {
  turbo_stream_t *s = (turbo_stream_t *)op->owner;
  int status = op->status;
  free(op);

  if (status == 0) {
    stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
    /* Enable send/recv on connected socket */
    setsockopt(st->base.socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
    s->connected = 1;

    /* Start receiving */
    if (s->on_recv || s->on_connect) {
      stream_iocp_submit_recv(s);
    }
  }

  if (s->on_connect) {
    s->on_connect(s, status, NULL);
  }
}

static void stream_iocp_handle_send(stream_iocp_op_t *op) {
  turbo_stream_t *s = (turbo_stream_t *)op->owner;
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  int status = op->status;

  if (op->buffer && op->owns_buffer) {
    mem_unref(op->buffer);
  }
  free(op);

  st->send_inflight = 0;

  if (s->on_write_complete) {
    s->on_write_complete(s, status);
  }

  /* If more data queued, flush it */
  if (s->send_head && !s->closing) {
    stream_iocp_submit_send(s);
  }
}

static void stream_iocp_handle_recv(stream_iocp_op_t *op) {
  turbo_stream_t *s = (turbo_stream_t *)op->owner;
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  int status = op->status;
  DWORD bytes = op->bytes_transferred;
  free(op);

  st->recv_inflight = 0;

  if (status != 0 || bytes == 0) {
    /* Error or graceful close */
    if (s->on_recv) {
      s->on_recv(s, NULL, NULL);
    }
    turbo_stream_close(s);
    return;
  }

  /* Deliver data via slice */
  mem_buffer_t *buf = s->recv_buf[s->recv_toggle];
  mem_slice_t slice;
  slice.data = buf->data;
  slice.length = bytes;
  slice.buffer = buf;
  mem_ref(buf);

  s->recv_toggle ^= 1;

  if (s->on_recv) {
    int close_requested = s->on_recv(s, &slice, NULL);
    mem_slice_release(&slice);
    if (close_requested) {
      turbo_stream_close(s);
      return;
    }
  } else {
    mem_slice_release(&slice);
  }

  /* Re-arm recv */
  if (st->recv_started && !s->closing) {
    stream_iocp_submit_recv(s);
  }
}

static void stream_iocp_handle_accept(stream_iocp_op_t *op) {
  turbo_stream_listener_t *l = (turbo_stream_listener_t *)op->owner;
  stream_iocp_server_state_t *st = (stream_iocp_server_state_t *)l->backend_data;
  int status = op->status;
  SOCKET client_socket = op->client_socket;

  InterlockedDecrement(&st->accepts_posted);

  if (status == 0) {
    /* Update client socket context */
    setsockopt(client_socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
               (const char *)&st->base.socket, sizeof(st->base.socket));

    /* Extract addresses */
    struct sockaddr *p_local = NULL, *p_remote = NULL;
    int local_len = 0, remote_len = 0;
    st->get_accept_ex_sockaddrs(op->accept_buf, 0,
                                sizeof(struct sockaddr_storage) + 16,
                                sizeof(struct sockaddr_storage) + 16,
                                &p_local, &local_len,
                                &p_remote, &remote_len);

    /* Create new turbo_stream_t for the client */
    turbo_stream_t *s = turbo_stream_create(l->ctx, l->kind);
    if (s) {
      iocp_init_with_socket(s, client_socket);
      s->connected = 1;
      s->listener = l;
      l->active_connections++;
      if (l->on_accept) {
        l->on_accept(l, s, p_remote);
      }
    } else {
      closesocket(client_socket);
    }
  } else {
    if (client_socket != INVALID_SOCKET) {
      closesocket(client_socket);
    }
  }

  /* Keep a fixed number of accepts outstanding on the single listener. */
  while (st->base.stopping == 0 &&
         InterlockedCompareExchange(&st->accepts_posted, 0, 0) < st->accept_depth) {
    if (stream_iocp_submit_accept(l) != 0) {
      break;
    }
  }

  free(op);
}

/* ── Submit helpers ───────────────────────────────────────── */

static int stream_iocp_submit_recv(turbo_stream_t *s) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  if (st->recv_inflight) return 0;

  stream_iocp_op_t *op = (stream_iocp_op_t *)calloc(1, sizeof(stream_iocp_op_t));
  if (!op) return TURBO_ENOMEM;

  op->kind = STREAM_IOCP_OP_RECV;
  op->owner = s;

  mem_buffer_t *buf = s->recv_buf[s->recv_toggle];
  WSABUF wsabuf;
  wsabuf.buf = buf->data;
  wsabuf.len = (ULONG)buf->capacity;
  op->flags = 0;

  st->recv_inflight = 1;
  InterlockedIncrement(&st->base.inflight_count);

  int rc = WSARecv(st->base.socket, &wsabuf, 1, NULL, &op->flags, &op->overlapped, NULL);
  if (rc == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err != WSA_IO_PENDING) {
      InterlockedDecrement(&st->base.inflight_count);
      st->recv_inflight = 0;
      free(op);
      return -(int)err;
    }
  }
  return 0;
}

static int stream_iocp_submit_send(turbo_stream_t *s) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  if (st->send_inflight || !s->send_head) return 0;

  /* Dequeue one buffer */
  mem_buffer_t *buf = s->send_head;
  s->send_head = buf->next;
  if (!s->send_head) s->send_tail = NULL;
  s->send_queued -= buf->used;
  buf->next = NULL;

  stream_iocp_op_t *op = (stream_iocp_op_t *)calloc(1, sizeof(stream_iocp_op_t));
  if (!op) {
    /* Re-enqueue */
    buf->next = s->send_head;
    s->send_head = buf;
    if (!s->send_tail) s->send_tail = buf;
    s->send_queued += buf->used;
    return TURBO_ENOMEM;
  }

  op->kind = STREAM_IOCP_OP_SEND;
  op->owner = s;
  op->buffer = buf;
  op->owns_buffer = 1;
  op->length = buf->used;

  WSABUF wsabuf;
  wsabuf.buf = buf->data;
  wsabuf.len = (ULONG)buf->used;

  st->send_inflight = 1;
  InterlockedIncrement(&st->base.inflight_count);

  int rc = WSASend(st->base.socket, &wsabuf, 1, NULL, 0, &op->overlapped, NULL);
  if (rc == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err != WSA_IO_PENDING) {
      InterlockedDecrement(&st->base.inflight_count);
      st->send_inflight = 0;
      free(op);
      mem_unref(buf);
      /* Re-enqueue */
      buf->next = s->send_head;
      s->send_head = buf;
      if (!s->send_tail) s->send_tail = buf;
      s->send_queued += buf->used;
      return -(int)err;
    }
  }
  return 0;
}

static int stream_iocp_submit_accept(turbo_stream_listener_t *l) {
  stream_iocp_server_state_t *st = (stream_iocp_server_state_t *)l->backend_data;

  stream_iocp_op_t *op = (stream_iocp_op_t *)calloc(1, sizeof(stream_iocp_op_t));
  if (!op) return TURBO_ENOMEM;

  op->kind = STREAM_IOCP_OP_ACCEPT;
  op->owner = l;
  op->client_socket = WSASocketW(st->accept_family, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                                 WSA_FLAG_OVERLAPPED);
  if (op->client_socket == INVALID_SOCKET) {
    free(op);
    return -(int)WSAGetLastError();
  }

  InterlockedIncrement(&st->accepts_posted);
  InterlockedIncrement(&st->base.inflight_count);
  BOOL ok = st->accept_ex(st->base.socket, op->client_socket, op->accept_buf, 0,
                           sizeof(struct sockaddr_storage) + 16,
                           sizeof(struct sockaddr_storage) + 16,
                           NULL, &op->overlapped);
  if (!ok) {
    int err = WSAGetLastError();
    if (err != ERROR_IO_PENDING) {
      InterlockedDecrement(&st->accepts_posted);
      InterlockedDecrement(&st->base.inflight_count);
      closesocket(op->client_socket);
      free(op);
      return -(int)err;
    }
  }
  return 0;
}

/* ── Backend ops implementation ───────────────────────────── */

static int iocp_setup_socket(stream_iocp_state_t *st, SOCKET s) {
  st->base.socket = s;

  /* Associate with IOCP. Key = 0 because it's a dedicated worker. */
  if (!CreateIoCompletionPort((HANDLE)s, st->base.completion_port, 0, 0)) {
    return -(int)GetLastError();
  }

  /* Load ConnectEx */
  GUID guid = WSAID_CONNECTEX;
  DWORD bytes;
  WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid,
           sizeof(guid), &st->connect_ex, sizeof(st->connect_ex),
           &bytes, NULL, NULL);

  /* TCP_NODELAY */
  int yes = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));

  return 0;
}

static int iocp_init_with_socket(turbo_stream_t *s, SOCKET existing) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  if (st) {
    if (existing != INVALID_SOCKET) {
      if (iocp_setup_socket(st, existing) != 0) return TURBO_ENOSYS;
      st->socket_bound = 1;
    }
    return 0;
  }

  st = (stream_iocp_state_t *)calloc(1, sizeof(stream_iocp_state_t));
  if (!st) return TURBO_ENOMEM;

  st->base.owner = s;
  st->base.ctx = s->ctx;
  st->base.socket = INVALID_SOCKET;

  /* Queue size: 1024 pointers (8KB on 64-bit) */
  st->base.queue_data = (uint8_t *)calloc(1024, sizeof(void *));
  if (!st->base.queue_data) {
    free(st);
    return TURBO_ENOMEM;
  }
  ring_spsc_init(&st->base.queue, st->base.queue_data, 1024 * sizeof(void *));

  st->base.completion_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
  if (!st->base.completion_port) {
    if (st->base.queue_data) free(st->base.queue_data);
    free(st);
    return TURBO_ENOMEM;
  }

  if (existing != INVALID_SOCKET) {
    if (iocp_setup_socket(st, existing) != 0) {
      CloseHandle(st->base.completion_port);
      if (st->base.queue_data) free(st->base.queue_data);
      free(st);
      return TURBO_ENOSYS;
    }
    st->socket_bound = 1;
  }

  st->base.worker_thread = CreateThread(NULL, 0, stream_iocp_worker, st, 0, NULL);
  if (!st->base.worker_thread) {
    if (st->base.socket != INVALID_SOCKET) closesocket(st->base.socket);
    CloseHandle(st->base.completion_port);
    if (st->base.queue_data) free(st->base.queue_data);
    free(st);
    return TURBO_ENOMEM;
  }

  coro_context_acquire_external(st->base.ctx);
  s->backend_data = st;
  return 0;
}

static int iocp_init(turbo_stream_t *s) {
  return iocp_init_with_socket(s, INVALID_SOCKET);
}

static int iocp_create_socket(stream_iocp_state_t *st, int af) {
  if (st->base.socket != INVALID_SOCKET) return 0;

  SOCKET s = WSASocketW(af, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
  if (s == INVALID_SOCKET) return -(int)WSAGetLastError();

  return iocp_setup_socket(st, s);
}

static int iocp_connect(turbo_stream_t *s, const struct sockaddr *addr) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  int af = addr->sa_family;
  int rc;

  rc = iocp_create_socket(st, af);
  if (rc != 0) return rc;

  /* ConnectEx requires a bound socket */
  if (!st->socket_bound) {
    struct sockaddr_storage bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    if (af == AF_INET6) {
      struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&bind_addr;
      a6->sin6_family = AF_INET6;
      a6->sin6_addr = in6addr_any;
      a6->sin6_port = 0;
      rc = bind(st->base.socket, (struct sockaddr *)a6, sizeof(*a6));
    } else {
      struct sockaddr_in *a4 = (struct sockaddr_in *)&bind_addr;
      a4->sin_family = AF_INET;
      a4->sin_addr.s_addr = INADDR_ANY;
      a4->sin_port = 0;
      rc = bind(st->base.socket, (struct sockaddr *)a4, sizeof(*a4));
    }
    if (rc == SOCKET_ERROR) return -(int)WSAGetLastError();
    st->socket_bound = 1;
  }

  /* Issue ConnectEx */
  stream_iocp_op_t *op = (stream_iocp_op_t *)calloc(1, sizeof(stream_iocp_op_t));
  if (!op) return TURBO_ENOMEM;

  op->kind = STREAM_IOCP_OP_CONNECT;
  op->owner = s;

  int addr_len = (af == AF_INET6) ? sizeof(struct sockaddr_in6)
                                   : sizeof(struct sockaddr_in);

  InterlockedIncrement(&st->base.inflight_count);
  BOOL ok = st->connect_ex(st->base.socket, addr, addr_len, NULL, 0, NULL,
                            &op->overlapped);
  if (!ok) {
    int err = WSAGetLastError();
    if (err != ERROR_IO_PENDING) {
      InterlockedDecrement(&st->base.inflight_count);
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
  /* Delegate to the public turbo_stream_send which uses get_send_buffer */
  mem_buffer_t *buf = mem_get_buffer(s->arena, len);
  if (!buf) return TURBO_ENOMEM;
  memcpy(buf->data, data, len);
  mem_set_used(buf, len);
  turbo_stream_enqueue_buffer(s, buf);
  mem_unref(buf); /* enqueue took a ref */
  return stream_iocp_submit_send(s);
}

static int iocp_flush(turbo_stream_t *s) {
  return stream_iocp_submit_send(s);
}

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
static void stream_iocp_shutdown_task(void *arg1, void *arg2) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)arg1;
  turbo_stream_t *s = (turbo_stream_t *)arg2;

  if (InterlockedCompareExchange(&st->base.inflight_count, 0, 0) > 0 ||
      InterlockedCompareExchange(&st->base.active_ticks, 0, 0) > 0) {
    coro_post(st->base.ctx, stream_iocp_shutdown_task, st, s);
    return;
  }

  if (st->base.worker_thread) {
    PostQueuedCompletionStatus(st->base.completion_port, 0, 0, NULL);
    WaitForSingleObject(st->base.worker_thread, 1000);
    CloseHandle(st->base.worker_thread);
    st->base.worker_thread = NULL;
  }

  if (st->base.completion_port) {
    CloseHandle(st->base.completion_port);
    st->base.completion_port = NULL;
  }

  if (st->base.queue_data) free(st->base.queue_data);
  coro_context_release_external(st->base.ctx);
  free(st);
  s->backend_data = NULL;
  turbo_stream_finalize_close(s);
}

static void iocp_close(turbo_stream_t *s) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  if (!st) { turbo_stream_finalize_close(s); return; }

  InterlockedExchange(&st->base.stopping, 1);
  if (st->base.socket != INVALID_SOCKET) {
    closesocket(st->base.socket);
    st->base.socket = INVALID_SOCKET;
  }
  coro_post(st->base.ctx, stream_iocp_shutdown_task, st, s);
}

static int iocp_get_local_addr(turbo_stream_t *s,
                                 struct sockaddr_storage *addr) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  if (!st || st->base.socket == INVALID_SOCKET) return TURBO_EINVAL;
  int len = sizeof(struct sockaddr_storage);
  if (getsockname(st->base.socket, (struct sockaddr *)addr, &len) == SOCKET_ERROR) {
    return -(int)WSAGetLastError();
  }
  return 0;
}

static int iocp_get_peer_addr(turbo_stream_t *s,
                                struct sockaddr_storage *addr) {
  stream_iocp_state_t *st = (stream_iocp_state_t *)s->backend_data;
  if (!st || st->base.socket == INVALID_SOCKET) return TURBO_EINVAL;
  int len = sizeof(struct sockaddr_storage);
  if (getpeername(st->base.socket, (struct sockaddr *)addr, &len) == SOCKET_ERROR) {
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

  SOCKET s = socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return TURBO_ENOSYS;

  /* Set REUSEADDR to prevent EADDRINUSE after rapid restarts */
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));

  if (bind(s, addr, (int)(addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6))) == SOCKET_ERROR) {
    closesocket(s);
    return TURBO_EADDRINUSE;
  }

  stream_iocp_server_state_t *st = calloc(1, sizeof(stream_iocp_server_state_t));
  if (!st) { closesocket(s); return TURBO_ENOMEM; }

  st->base.owner = l;
  st->base.ctx = l->ctx;
  st->base.socket = s;
  st->accept_family = addr->sa_family;
  st->accept_depth = STREAM_IOCP_ACCEPT_DEPTH;

  /* Queue size: 1024 pointers */
  st->base.queue_data = (uint8_t *)calloc(1024, sizeof(void *));
  if (!st->base.queue_data) {
    closesocket(s);
    free(st);
    return TURBO_ENOMEM;
  }
  ring_spsc_init(&st->base.queue, st->base.queue_data, 1024 * sizeof(void *));

  /* Get AcceptEx pointers */
  GUID accept_guid = WSAID_ACCEPTEX;
  GUID getaddr_guid = WSAID_GETACCEPTEXSOCKADDRS;
  DWORD bytes;
  WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &accept_guid, sizeof(accept_guid),
           &st->accept_ex, sizeof(st->accept_ex), &bytes, NULL, NULL);
  WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &getaddr_guid, sizeof(getaddr_guid),
           &st->get_accept_ex_sockaddrs, sizeof(st->get_accept_ex_sockaddrs), &bytes, NULL, NULL);

  l->backend_data = st;
  return 0;
}

static int iocp_listen(turbo_stream_listener_t *l, int backlog) {
  stream_iocp_server_state_t *st = (stream_iocp_server_state_t *)l->backend_data;
  int posted = 0;
  if (!st) return TURBO_EINVAL;

  if (listen(st->base.socket, backlog) == SOCKET_ERROR) {
    return TURBO_ENOSYS;
  }

  st->base.completion_port = CreateIoCompletionPort((HANDLE)st->base.socket, NULL, (ULONG_PTR)st, 0);
  if (!st->base.completion_port) return TURBO_ENOSYS;

  st->base.worker_thread = CreateThread(NULL, 0, stream_iocp_worker, st, 0, NULL);
  if (!st->base.worker_thread) return TURBO_ENOSYS;

  coro_context_acquire_external(st->base.ctx);
  /* Pre-post multiple accepts so a single listener can absorb bursts. */
  while (posted < st->accept_depth) {
    int rc = stream_iocp_submit_accept(l);
    if (rc != 0) {
      return posted > 0 ? 0 : rc;
    }
    posted++;
  }
  return 0;
}

static void iocp_listener_shutdown_task(void *arg1, void *arg2) {
  stream_iocp_server_state_t *st = (stream_iocp_server_state_t *)arg1;
  turbo_stream_listener_t *l = (turbo_stream_listener_t *)arg2;

  if (InterlockedCompareExchange(&st->base.inflight_count, 0, 0) > 0 ||
      InterlockedCompareExchange(&st->base.active_ticks, 0, 0) > 0) {
    coro_post(st->base.ctx, iocp_listener_shutdown_task, st, l);
    return;
  }

  if (st->base.worker_thread) {
    PostQueuedCompletionStatus(st->base.completion_port, 0, 0, NULL);
    WaitForSingleObject(st->base.worker_thread, 1000);
    CloseHandle(st->base.worker_thread);
    st->base.worker_thread = NULL;
  }

  if (st->base.completion_port) {
    CloseHandle(st->base.completion_port);
    st->base.completion_port = NULL;
  }

  /* Cleanup pending ops in queue - logic remains same but uses SPSC pop */
  stream_iocp_op_t *chain = queue_pop_all(st);
  while (chain) {
    stream_iocp_op_t *next = chain->next;
    if (chain->client_socket != INVALID_SOCKET) closesocket(chain->client_socket);
    free(chain);
    chain = next;
  }

  if (st->base.queue_data) free(st->base.queue_data);
  coro_context_release_external(st->base.ctx);
  free(st);
  l->backend_data = NULL;
  turbo_stream_listener_finalize_close(l);
}

static void iocp_listener_close(turbo_stream_listener_t *l) {
  stream_iocp_server_state_t *st = (stream_iocp_server_state_t *)l->backend_data;
  if (!st) { turbo_stream_listener_finalize_close(l); return; }

  InterlockedExchange(&st->base.stopping, 1);
  if (st->base.socket != INVALID_SOCKET) {
    closesocket(st->base.socket);
    st->base.socket = INVALID_SOCKET;
  }
  coro_post(st->base.ctx, iocp_listener_shutdown_task, st, l);
}

const turbo_stream_backend_ops_t turbo_stream_iocp_ops = {
  .init          = iocp_init,
  .connect       = iocp_connect,
  .connect_pipe  = iocp_connect_pipe,
  .send          = iocp_send,
  .flush         = iocp_flush,
  .recv_start    = iocp_recv_start,
  .recv_stop     = iocp_recv_stop,
  .close         = iocp_close,
  .get_local_addr = iocp_get_local_addr,
  .get_peer_addr  = iocp_get_peer_addr,
  .bind          = iocp_bind,
  .bind_pipe     = iocp_bind_pipe,
  .listen        = iocp_listen,
  .listener_close = iocp_listener_close,
};
