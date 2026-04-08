/**
 * @file turbo_stream_pipe_win.c
 * @brief Windows named pipe backend for turbo_stream_t.
 *
 * Uses CreateNamedPipe + IOCP for server, CreateFile + IOCP for client.
 * This replaces the libuv-based turbo_pipe.c on Windows.
 */

#include "turbo_stream_internal.h"
#include "turbo_thread.h"
#include <tlog.h>
#include <windows.h>
#include <winsock2.h>

#include <stdlib.h>
#include <string.h>

/* ── Pipe IOCP state ──────────────────────────────────────── */

typedef enum { PIPE_OP_CONNECT = 1, PIPE_OP_WRITE = 2, PIPE_OP_READ = 3 } pipe_op_kind_t;

typedef struct pipe_op_s {
  OVERLAPPED overlapped;
  pipe_op_kind_t kind;
  turbo_stream_t *stream;
  struct pipe_op_s *next;
  mem_buffer_t *buffer;
  DWORD bytes_transferred;
  int status;
} pipe_op_t;

typedef struct pipe_state_s {
  turbo_stream_t *stream;
  coro_context_t *ctx;
  HANDLE pipe_handle;
  HANDLE completion_port;
  HANDLE worker_thread;
  HANDLE connect_thread;
  volatile LONG stopping;
  volatile LONG close_requested;
  volatile LONG close_finalized;
  volatile LONG pending_posts;
  CRITICAL_SECTION queue_lock;
  int queue_lock_initialized;
  pipe_op_t *completed_head;
  pipe_op_t *completed_tail;
  int recv_started;
  int recv_inflight;
  int send_inflight;
} pipe_state_t;

static void pipe_queue_push(pipe_state_t *st, pipe_op_t *op) {
  op->next = NULL;
  EnterCriticalSection(&st->queue_lock);
  if (st->completed_tail) {
    st->completed_tail->next = op;
  } else {
    st->completed_head = op;
  }
  st->completed_tail = op;
  LeaveCriticalSection(&st->queue_lock);
}

static pipe_op_t *pipe_queue_pop_all(pipe_state_t *st) {
  EnterCriticalSection(&st->queue_lock);
  pipe_op_t *head = st->completed_head;
  st->completed_head = NULL;
  st->completed_tail = NULL;
  LeaveCriticalSection(&st->queue_lock);
  return head;
}

static void pipe_tick(void *arg1, void *arg2);
static int pipe_submit_read(turbo_stream_t *s);
static int pipe_submit_write(turbo_stream_t *s);

#define PIPE_CONNECT_WAIT_SLICE_MS 100

typedef struct pipe_connect_task_s {
  pipe_state_t *state;
  turbo_stream_t *stream;
  char *name;
} pipe_connect_task_t;

static int pipe_post_wait(coro_context_t *ctx, coro_post_fn fn, void *arg1, void *arg2,
                          volatile LONG *stopping) {
  int rc;

  for (;;) {
    rc = coro_post(ctx, fn, arg1, arg2);
    if (rc == 0) {
      turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(ctx));
      return 0;
    }
    if (stopping && InterlockedCompareExchange(stopping, 0, 0) != 0) {
      return TURBO_ECANCELED;
    }

    turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(ctx));
    turbo_thread_yield();
  }
}

static void pipe_free_chain(pipe_op_t *chain) {
  while (chain) {
    pipe_op_t *next = chain->next;
    if (chain->buffer) mem_unref(chain->buffer);
    free(chain);
    chain = next;
  }
}

static void pipe_finalize_close(pipe_state_t *st) {
  turbo_stream_t *s;

  if (!st) return;
  if (InterlockedCompareExchange(&st->close_finalized, 1, 0) != 0) return;

  s = st->stream;
  pipe_free_chain(pipe_queue_pop_all(st));

  if (st->queue_lock_initialized) {
    DeleteCriticalSection(&st->queue_lock);
    st->queue_lock_initialized = 0;
  }

  if (s) {
    s->backend_data = NULL;
  }

  if (st->completion_port) {
    CloseHandle(st->completion_port);
    st->completion_port = NULL;
  }

  free(st);

  if (s) {
    turbo_stream_finalize_close(s);
  }
}

static void pipe_maybe_finalize_close(pipe_state_t *st) {
  if (!st) return;
  if (!InterlockedCompareExchange(&st->close_requested, 0, 0) &&
      !InterlockedCompareExchange(&st->stopping, 0, 0)) {
    return;
  }
  if (InterlockedCompareExchange(&st->pending_posts, 0, 0) != 0) return;

  if (st->worker_thread) {
    if (WaitForSingleObject(st->worker_thread, 0) == WAIT_TIMEOUT) {
      /* Thread still exiting, check again next tick */
      InterlockedIncrement(&st->pending_posts);
      if (coro_post(st->ctx, pipe_tick, st, NULL) == 0) {
        /* PROACTIVE WAKE: Ensure the main loop isn't sleeping and processes this tick */
        void *loop = coro_context_native_loop(st->ctx);
        if (loop) turbo_loop_wake((turbo_loop_t *)loop);
      } else {
        InterlockedDecrement(&st->pending_posts);
      }
      return;
    }
    CloseHandle(st->worker_thread);
    st->worker_thread = NULL;
  }

  if (st->connect_thread) {
    if (WaitForSingleObject(st->connect_thread, 0) == WAIT_TIMEOUT) {
      InterlockedIncrement(&st->pending_posts);
      if (coro_post(st->ctx, pipe_tick, st, NULL) == 0) {
        void *loop = coro_context_native_loop(st->ctx);
        if (loop) turbo_loop_wake((turbo_loop_t *)loop);
      } else {
        InterlockedDecrement(&st->pending_posts);
      }
      return;
    }
    CloseHandle(st->connect_thread);
    st->connect_thread = NULL;
  }

  pipe_finalize_close(st);
}

static int pipe_publish_connect_result(pipe_connect_task_t *task, HANDLE pipe_handle, int status) {
  pipe_state_t *st;
  pipe_op_t *op;

  if (!task) return TURBO_EINVAL;

  st = task->state;
  if (!st) return TURBO_EINVAL;

  op = (pipe_op_t *)calloc(1, sizeof(pipe_op_t));
  if (!op) {
    if (pipe_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(pipe_handle);
    }
    return TURBO_ENOMEM;
  }

  op->kind = PIPE_OP_CONNECT;
  op->stream = task->stream;
  op->status = status;

  if (status == 0) {
    st->pipe_handle = pipe_handle;
  }

  pipe_queue_push(st, op);
  InterlockedIncrement(&st->pending_posts);
  if (pipe_post_wait(st->ctx, pipe_tick, st, NULL, &st->stopping) != 0) {
    InterlockedDecrement(&st->pending_posts);
    if (status == 0 && pipe_handle != INVALID_HANDLE_VALUE) {
      st->pipe_handle = INVALID_HANDLE_VALUE;
      CloseHandle(pipe_handle);
    }
    free(op);
    return TURBO_ECANCELED;
  }

  return 0;
}

static DWORD WINAPI pipe_connect_worker(LPVOID arg) {
  pipe_connect_task_t *task = (pipe_connect_task_t *)arg;
  pipe_state_t *st;
  HANDLE pipe_handle = INVALID_HANDLE_VALUE;
  DWORD last_error = ERROR_FILE_NOT_FOUND;
  int status = 0;

  if (!task) {
    return 0;
  }

  st = task->state;

  while (!InterlockedCompareExchange(&st->stopping, 0, 0)) {
    pipe_handle = CreateFileA(task->name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, NULL);
    if (pipe_handle != INVALID_HANDLE_VALUE) {
      break;
    }

    last_error = GetLastError();
    if (last_error != ERROR_FILE_NOT_FOUND && last_error != ERROR_PIPE_BUSY) {
      status = -(int)last_error;
      break;
    }

    if (!WaitNamedPipeA(task->name, PIPE_CONNECT_WAIT_SLICE_MS)) {
      last_error = GetLastError();
      if (last_error != ERROR_FILE_NOT_FOUND &&
          last_error != ERROR_PIPE_BUSY &&
          last_error != ERROR_SEM_TIMEOUT) {
        status = -(int)last_error;
        break;
      }
    }
  }

  if (status == 0 && InterlockedCompareExchange(&st->stopping, 0, 0) != 0) {
    status = TURBO_ECANCELED;
  }

  if (status == 0 && pipe_handle == INVALID_HANDLE_VALUE) {
    status = -(int)last_error;
  }

  if (status == 0 &&
      !CreateIoCompletionPort(pipe_handle, st->completion_port, 0, 0)) {
    status = -(int)GetLastError();
  }

  if (status != 0 && pipe_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(pipe_handle);
    pipe_handle = INVALID_HANDLE_VALUE;
  }

  pipe_publish_connect_result(task, pipe_handle, status);

  free(task->name);
  free(task);
  return 0;
}

static DWORD WINAPI pipe_worker(LPVOID arg) {
  pipe_state_t *st = (pipe_state_t *)arg;
  DWORD bytes;
  ULONG_PTR key;
  OVERLAPPED *ov;

  while (!InterlockedCompareExchange(&st->stopping, 0, 0)) {
    BOOL ok = GetQueuedCompletionStatus(st->completion_port, &bytes, &key, &ov, 1);
    if (!ov) {
      if (!ok && GetLastError() == WAIT_TIMEOUT) continue;
      break;
    }
    pipe_op_t *op = CONTAINING_RECORD(ov, pipe_op_t, overlapped);
    op->bytes_transferred = bytes;
    op->status = ok ? 0 : -(int)GetLastError();
    pipe_queue_push(st, op);
    InterlockedIncrement(&st->pending_posts);
    if (pipe_post_wait(st->ctx, pipe_tick, st, NULL, &st->stopping) != 0) {
      InterlockedDecrement(&st->pending_posts);
    }
  }
  return 0;
}

static void pipe_handle_connect(pipe_op_t *op) {
  turbo_stream_t *s = op->stream;
  int status = op->status;
  free(op);
  if (status == 0) s->connected = 1;
  if (s->on_connect) s->on_connect(s, status, NULL);
  if (status == 0) pipe_submit_read(s);
}

static void pipe_handle_write(pipe_op_t *op) {
  turbo_stream_t *s = op->stream;
  pipe_state_t *st = (pipe_state_t *)s->backend_data;
  if (op->buffer) mem_unref(op->buffer);
  free(op);
  st->send_inflight = 0;
  if (s->on_write_complete) s->on_write_complete(s, 0);
  if (s->send_head && !s->closing) pipe_submit_write(s);
}

static void pipe_handle_read(pipe_op_t *op) {
  turbo_stream_t *s = op->stream;
  pipe_state_t *st = (pipe_state_t *)s->backend_data;
  int status = op->status;
  DWORD bytes = op->bytes_transferred;
  free(op);
  st->recv_inflight = 0;

  if (status != 0 || bytes == 0) {
    if (s->on_recv) s->on_recv(s, NULL, NULL);
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
    int close_req = s->on_recv(s, &slice, NULL);
    mem_slice_release(&slice);
    if (close_req) {
      turbo_stream_close(s);
      return;
    }
  } else {
    mem_slice_release(&slice);
  }

  if (st->recv_started && !s->closing) pipe_submit_read(s);
}

static void pipe_tick(void *arg1, void *arg2) {
  pipe_state_t *st = (pipe_state_t *)arg1;
  (void)arg2;
  pipe_op_t *chain = pipe_queue_pop_all(st);
  while (chain) {
    pipe_op_t *next = chain->next;
    switch (chain->kind) {
    case PIPE_OP_CONNECT:
      pipe_handle_connect(chain);
      break;
    case PIPE_OP_WRITE:
      pipe_handle_write(chain);
      break;
    case PIPE_OP_READ:
      pipe_handle_read(chain);
      break;
    }
    chain = next;
  }

  InterlockedDecrement(&st->pending_posts);
  pipe_maybe_finalize_close(st);
}

static int pipe_submit_read(turbo_stream_t *s) {
  pipe_state_t *st = (pipe_state_t *)s->backend_data;
  if (st->recv_inflight) return 0;

  pipe_op_t *op = (pipe_op_t *)calloc(1, sizeof(pipe_op_t));
  if (!op) return TURBO_ENOMEM;

  op->kind = PIPE_OP_READ;
  op->stream = s;

  mem_buffer_t *buf = s->recv_buf[s->recv_toggle];
  st->recv_inflight = 1;

  BOOL ok = ReadFile(st->pipe_handle, buf->data, (DWORD)buf->capacity, NULL, &op->overlapped);
  if (!ok) {
    DWORD err = GetLastError();
    if (err != ERROR_IO_PENDING) {
      st->recv_inflight = 0;
      free(op);
      return -(int)err;
    }
  }
  return 0;
}

static int pipe_submit_write(turbo_stream_t *s) {
  pipe_state_t *st = (pipe_state_t *)s->backend_data;
  if (st->send_inflight || !s->send_head) return 0;

  mem_buffer_t *buf = s->send_head;
  s->send_head = buf->next;
  if (!s->send_head) s->send_tail = NULL;
  s->send_queued -= buf->used;
  buf->next = NULL;

  pipe_op_t *op = (pipe_op_t *)calloc(1, sizeof(pipe_op_t));
  if (!op) {
    buf->next = s->send_head;
    s->send_head = buf;
    if (!s->send_tail) s->send_tail = buf;
    s->send_queued += buf->used;
    return TURBO_ENOMEM;
  }

  op->kind = PIPE_OP_WRITE;
  op->stream = s;
  op->buffer = buf; /* owns the ref from the queue */

  st->send_inflight = 1;

  BOOL ok = WriteFile(st->pipe_handle, buf->data, (DWORD)buf->used, NULL, &op->overlapped);
  if (!ok) {
    DWORD err = GetLastError();
    if (err != ERROR_IO_PENDING) {
      st->send_inflight = 0;
      op->buffer = NULL;
      free(op);
      buf->next = s->send_head;
      s->send_head = buf;
      if (!s->send_tail) s->send_tail = buf;
      s->send_queued += buf->used;
      return -(int)err;
    }
  }
  return 0;
}

/* ── Backend ops ──────────────────────────────────────────── */

static int pw_init(turbo_stream_t *s) {
  pipe_state_t *st = (pipe_state_t *)calloc(1, sizeof(pipe_state_t));
  if (!st) return TURBO_ENOMEM;

  st->stream = s;
  st->ctx = s->ctx;
  st->pipe_handle = INVALID_HANDLE_VALUE;

  InitializeCriticalSection(&st->queue_lock);
  st->queue_lock_initialized = 1;

  st->completion_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
  if (!st->completion_port) {
    DeleteCriticalSection(&st->queue_lock);
    free(st);
    return TURBO_ENOMEM;
  }

  st->worker_thread = CreateThread(NULL, 0, pipe_worker, st, 0, NULL);
  if (!st->worker_thread) {
    CloseHandle(st->completion_port);
    DeleteCriticalSection(&st->queue_lock);
    free(st);
    return TURBO_ENOMEM;
  }

  s->backend_data = st;
  return 0;
}

static int pw_connect_pipe(turbo_stream_t *s, const char *name) {
  pipe_state_t *st = (pipe_state_t *)s->backend_data;
  pipe_connect_task_t *task;
  size_t name_len;
  if (!st) return TURBO_EINVAL;

  if (st->connect_thread) {
    if (WaitForSingleObject(st->connect_thread, 0) == WAIT_TIMEOUT) {
      return TURBO_EALREADY;
    }
    CloseHandle(st->connect_thread);
    st->connect_thread = NULL;
  }

  if (st->pipe_handle != INVALID_HANDLE_VALUE) {
    return TURBO_EALREADY;
  }

  task = (pipe_connect_task_t *)calloc(1, sizeof(pipe_connect_task_t));
  if (!task) {
    return TURBO_ENOMEM;
  }

  name_len = strlen(name) + 1;
  task->name = (char *)malloc(name_len);
  if (!task->name) {
    free(task);
    return TURBO_ENOMEM;
  }
  memcpy(task->name, name, name_len);
  task->state = st;
  task->stream = s;

  st->connect_thread = CreateThread(NULL, 0, pipe_connect_worker, task, 0, NULL);
  if (!st->connect_thread) {
    free(task->name);
    free(task);
    return TURBO_ENOMEM;
  }

  return 0;
}

static int pw_connect(turbo_stream_t *s, const struct sockaddr *a) {
  UNUSED(s);
  UNUSED(a);
  return TURBO_EINVAL; /* Pipe doesn't do TCP connect */
}

static int pw_send(turbo_stream_t *s, const char *data, size_t len) {
  if (!s || !data || len == 0) return TURBO_EINVAL;
  mem_buffer_t *buf = mem_get_buffer(s->arena, len);
  if (!buf) return TURBO_ENOMEM;
  memcpy(buf->data, data, len);
  mem_set_used(buf, len);
  turbo_stream_enqueue_buffer(s, buf);
  mem_unref(buf);
  return pipe_submit_write(s);
}

static int pw_flush(turbo_stream_t *s) { return pipe_submit_write(s); }

static int pw_recv_start(turbo_stream_t *s) {
  pipe_state_t *st = (pipe_state_t *)s->backend_data;
  if (!st) return TURBO_EINVAL;
  st->recv_started = 1;
  return pipe_submit_read(s);
}

static void pw_recv_stop(turbo_stream_t *s) {
  pipe_state_t *st = (pipe_state_t *)s->backend_data;
}

static void pw_close(turbo_stream_t *s) {
  /* s->closing is already set by turbo_stream_close before dispatching here */
  pipe_state_t *st = (pipe_state_t *)s->backend_data;
  if (st) {
    InterlockedExchange(&st->stopping, 1);
    InterlockedExchange(&st->close_requested, 1);
    if (st->pipe_handle != INVALID_HANDLE_VALUE) {
      CancelIo(st->pipe_handle);
      CloseHandle(st->pipe_handle);
      st->pipe_handle = INVALID_HANDLE_VALUE;
    }
    if (st->worker_thread) {
      /* Wake up the worker thread immediately so it doesn't wait */
      PostQueuedCompletionStatus(st->completion_port, 0, 0, NULL);
    }
    /* Wake the main loop to process this close immediately */
    void *loop = coro_context_native_loop(st->ctx);
    if (loop) turbo_loop_wake((turbo_loop_t *)loop);
  }

  pipe_maybe_finalize_close(st);
}

static int pw_get_local(turbo_stream_t *s, struct sockaddr_storage *a) {
  UNUSED(s);
  UNUSED(a);
  return TURBO_ENOTSUP; /* Pipes don't have sockaddr */
}

static int pw_get_peer(turbo_stream_t *s, struct sockaddr_storage *a) {
  UNUSED(s);
  UNUSED(a);
  return TURBO_ENOTSUP;
}

/* ── Listener: CreateNamedPipe accept loop ────────────────── */

typedef struct pipe_listener_state_s {
  turbo_stream_listener_t *listener;
  coro_context_t *ctx;
  char *pipe_name;
  HANDLE cancel_event; /* signalled on shutdown to unblock */
  HANDLE worker_thread;
  volatile LONG stopping;
} pipe_listener_state_t;

typedef struct pipe_accept_notify_s {
  pipe_listener_state_t *lst;
  HANDLE pipe_handle; /* connected, NOT yet IOCP-associated */
} pipe_accept_notify_t;

static void listener_tick(void *arg1, void *arg2);
static void listener_fail_tick(void *arg1, void *arg2);

static void pipe_listener_discard_client(HANDLE hdl, turbo_stream_t *client, pipe_state_t *st) {
  if (st) {
    if (st->completion_port) {
      CloseHandle(st->completion_port);
      st->completion_port = NULL;
    }
    if (st->queue_lock_initialized) {
      DeleteCriticalSection(&st->queue_lock);
      st->queue_lock_initialized = 0;
    }
    free(st);
  }

  if (client) {
    if (client->recv_buf[0]) mem_unref(client->recv_buf[0]);
    if (client->recv_buf[1]) mem_unref(client->recv_buf[1]);
    free(client);
  }

  if (hdl && hdl != INVALID_HANDLE_VALUE) {
    CloseHandle(hdl);
  }
}

static void listener_fail_tick(void *arg1, void *arg2) {
  pipe_listener_state_t *lst = (pipe_listener_state_t *)arg1;
  const char *reason = (const char *)arg2;
  turbo_stream_listener_t *listener;

  if (!lst || !lst->listener) {
    return;
  }

  listener = lst->listener;
  if (listener->backend_data != lst) {
    return;
  }

  TLOG_ERROR("pipe listener accept failed: {}", reason ? reason : "unknown error");
  turbo_stream_listener_close(listener);
}

/**
 * @brief Accept loop: overlapped ConnectNamedPipe with a cancel event.
 *
 * The pipe handle is intentionally left unassociated with any IOCP here.
 * listener_tick performs the first (and only) IOCP association.
 */
static DWORD WINAPI listener_worker(LPVOID arg) {
  pipe_listener_state_t *lst = (pipe_listener_state_t *)arg;

  while (!InterlockedCompareExchange(&lst->stopping, 0, 0)) {
    /* FILE_FLAG_OVERLAPPED required for later async IO on accepted handle.
       Do NOT associate with any IOCP here. */
    HANDLE pipe_hdl = CreateNamedPipeA(lst->pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, NULL);
    if (pipe_hdl == INVALID_HANDLE_VALUE) break;

    /* Overlapped ConnectNamedPipe with hEvent = cancel_event so we can
       wake up on shutdown without closing the pipe prematurely. */
    OVERLAPPED ov = {0};
    ov.hEvent = lst->cancel_event;

    BOOL ok = ConnectNamedPipe(pipe_hdl, &ov);
    DWORD err = GetLastError();

    if (!ok) {
      if (err == ERROR_IO_PENDING) {
        DWORD w = WaitForSingleObject(lst->cancel_event, INFINITE);
        if (w != WAIT_OBJECT_0) {
          CancelIo(pipe_hdl);
          CloseHandle(pipe_hdl);
          break;
        }
        DWORD bytes = 0;
        BOOL got = GetOverlappedResult(pipe_hdl, &ov, &bytes, FALSE);
        if (!got || InterlockedCompareExchange(&lst->stopping, 0, 0)) {
          CancelIo(pipe_hdl);
          CloseHandle(pipe_hdl);
          break;
        }
        ResetEvent(lst->cancel_event);
      } else if (err == ERROR_PIPE_CONNECTED) {
        /* client arrived before ConnectNamedPipe — ok */
      } else {
        CloseHandle(pipe_hdl);
        continue;
      }
    }

    if (InterlockedCompareExchange(&lst->stopping, 0, 0)) {
      CloseHandle(pipe_hdl);
      break;
    }

    pipe_accept_notify_t *note = (pipe_accept_notify_t *)malloc(sizeof(pipe_accept_notify_t));
    if (note) {
      note->lst = lst;
      note->pipe_handle = pipe_hdl;
      if (pipe_post_wait(lst->ctx, listener_tick, note, NULL, &lst->stopping) != 0) {
        CloseHandle(pipe_hdl);
        free(note);
        if (!InterlockedCompareExchange(&lst->stopping, 0, 0)) {
          (void)pipe_post_wait(lst->ctx, listener_fail_tick, lst,
                               "failed to post named-pipe accept", &lst->stopping);
        }
        break;
      }
    } else {
      CloseHandle(pipe_hdl);
      if (!InterlockedCompareExchange(&lst->stopping, 0, 0)) {
        (void)pipe_post_wait(lst->ctx, listener_fail_tick, lst,
                             "failed to allocate named-pipe accept note", &lst->stopping);
      }
      break;
    }
  }
  return 0;
}

/**
 * @brief Called on the coro event-loop thread when a client connected.
 *
 * The pipe handle has never been associated with any IOCP, so this is the
 * first and only association.
 */
static void listener_tick(void *arg1, void *arg2) {
  (void)arg2;
  pipe_accept_notify_t *note = (pipe_accept_notify_t *)arg1;
  pipe_listener_state_t *lst = note->lst;
  HANDLE hdl = note->pipe_handle;
  free(note);

  turbo_stream_listener_t *listener = lst->listener;

  if (InterlockedCompareExchange(&lst->stopping, 0, 0)) {
    CloseHandle(hdl);
    return;
  }

  /* Allocate a new stream for the accepted connection */
  turbo_stream_t *client = (turbo_stream_t *)calloc(1, sizeof(turbo_stream_t));
  if (!client) {
    pipe_listener_discard_client(hdl, NULL, NULL);
    listener_fail_tick(lst, "failed to allocate accepted named-pipe stream");
    return;
  }

  const turbo_stream_backend_ops_t *ops = turbo_stream_resolve_backend(lst->ctx, TURBO_STREAM_PIPE);
  if (!ops) {
    pipe_listener_discard_client(hdl, client, NULL);
    listener_fail_tick(lst, "failed to resolve named-pipe backend");
    return;
  }

  if (turbo_stream_init_common(client, lst->ctx, TURBO_STREAM_PIPE, ops) != 0) {
    pipe_listener_discard_client(hdl, client, NULL);
    listener_fail_tick(lst, "failed to initialize accepted named-pipe stream");
    return;
  }

  /* Manually init backend state (like pw_init) but skip creating new thread/IOCP */
  pipe_state_t *st = (pipe_state_t *)calloc(1, sizeof(pipe_state_t));
  if (!st) {
    pipe_listener_discard_client(hdl, client, NULL);
    listener_fail_tick(lst, "failed to allocate accepted named-pipe state");
    return;
  }

  st->stream = client;
  st->ctx = lst->ctx;
  st->pipe_handle = hdl;

  InitializeCriticalSection(&st->queue_lock);
  st->queue_lock_initialized = 1;

  st->completion_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
  if (!st->completion_port) {
    pipe_listener_discard_client(hdl, client, st);
    listener_fail_tick(lst, "failed to create named-pipe completion port");
    return;
  }

  /* Associate the connected handle with the stream's IOCP */
  if (!CreateIoCompletionPort(hdl, st->completion_port, 0, 0)) {
    pipe_listener_discard_client(hdl, client, st);
    listener_fail_tick(lst, "failed to attach accepted named pipe to IOCP");
    return;
  }

  st->worker_thread = CreateThread(NULL, 0, pipe_worker, st, 0, NULL);
  if (!st->worker_thread) {
    pipe_listener_discard_client(hdl, client, st);
    listener_fail_tick(lst, "failed to start accepted named-pipe worker");
    return;
  }

  client->backend_data = st;
  client->connected = 1;
  client->listener = listener;
  listener->active_connections++;

  if (listener->on_accept) {
    listener->on_accept(listener, client, NULL);
  }
}

static int pw_bind(turbo_stream_listener_t *l, const struct sockaddr *a) {
  UNUSED(l);
  UNUSED(a);
  return TURBO_EINVAL;
}

static int pw_bind_pipe(turbo_stream_listener_t *l, const char *name) {
  if (!l || !name) return TURBO_EINVAL;
  size_t len = strlen(name);
  char *stored = (char *)calloc(1, len + 1);
  if (!stored) return TURBO_ENOMEM;
  memcpy(stored, name, len);
  l->backend_data = stored;
  return 0;
}

static int pw_listen(turbo_stream_listener_t *l, int backlog) {
  UNUSED(backlog);
  char *stored_name;
  if (!l || !l->backend_data) return TURBO_EINVAL;

  stored_name = (char *)l->backend_data;
  const char *pipe_name = stored_name;

  pipe_listener_state_t *lst = (pipe_listener_state_t *)calloc(1, sizeof(pipe_listener_state_t));
  if (!lst) {
    free(stored_name);
    l->backend_data = NULL;
    return TURBO_ENOMEM;
  }

  size_t len = strlen(pipe_name);
  lst->pipe_name = (char *)calloc(1, len + 1);
  if (!lst->pipe_name) {
    free(lst);
    free(stored_name);
    l->backend_data = NULL;
    return TURBO_ENOMEM;
  }
  memcpy(lst->pipe_name, pipe_name, len);

  lst->listener = l;
  lst->ctx = l->ctx;

  /* cancel_event: auto-reset, initially non-signalled */
  lst->cancel_event = CreateEventA(NULL, FALSE, FALSE, NULL);
  if (!lst->cancel_event) {
    free(lst->pipe_name);
    free(lst);
    free(stored_name);
    l->backend_data = NULL;
    return TURBO_ENOMEM;
  }

  /* Replace the plain name pointer stored by bind_pipe with our full state */
  free(stored_name);
  l->backend_data = lst;

  lst->worker_thread = CreateThread(NULL, 0, listener_worker, lst, 0, NULL);
  if (!lst->worker_thread) {
    CloseHandle(lst->cancel_event);
    free(lst->pipe_name);
    free(lst);
    l->backend_data = NULL;
    return TURBO_ENOMEM;
  }

  return 0;
}

static void pw_listener_close(turbo_stream_listener_t *l) {
  if (!l->backend_data) {
    turbo_stream_listener_finalize_close(l);
    return;
  }

  pipe_listener_state_t *lst = (pipe_listener_state_t *)l->backend_data;
  l->backend_data = NULL;

  InterlockedExchange(&lst->stopping, 1);

  /* Signal the event: wakes WaitForSingleObject in the worker */
  if (lst->cancel_event) {
    SetEvent(lst->cancel_event);
    if (lst->worker_thread) {
      WaitForSingleObject(lst->worker_thread, 3000);
      CloseHandle(lst->worker_thread);
      lst->worker_thread = NULL;
    }
    CloseHandle(lst->cancel_event);
    lst->cancel_event = NULL;
  }

  free(lst->pipe_name);
  free(lst);

  turbo_stream_listener_finalize_close(l);
}

const turbo_stream_backend_ops_t turbo_stream_pipe_win_ops = {
    .init = pw_init,
    .connect = pw_connect,
    .connect_pipe = pw_connect_pipe,
    .send = pw_send,
    .flush = pw_flush,
    .recv_start = pw_recv_start,
    .recv_stop = pw_recv_stop,
    .close = pw_close,
    .get_local_addr = pw_get_local,
    .get_peer_addr = pw_get_peer,
    .bind = pw_bind,
    .bind_pipe = pw_bind_pipe,
    .listen = pw_listen,
    .listener_close = pw_listener_close,
};
