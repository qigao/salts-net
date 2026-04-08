/**
 * @file turbo_iocp_pool.c
 * @brief Shared IOCP completion port pool — one pool per coro_context_t.
 *
 * DESIGN:
 * - Single IOCP port, multiple worker threads dequeue completions
 * - Workers publish pointers into a disruptor MPMC ring (lock-free)
 * - Loop thread drains the disruptor via non-blocking consumer poll
 * - Each IOCP op carries its own owner pointer → no completion-key lookup
 *
 * THREAD MODEL:
 * - Workers: GetQueuedCompletionStatus → disruptor publish → wake loop
 * - Loop:    disruptor consume → dispatch to stream/datagram handler
 */

#ifdef _WIN32

#include "turbo_iocp_pool.h"
#include "CoroNet/turbo_coro_internal.h"
#include "disruptor.h"
#include "turbo_thread.h"
#include "tlog.h"
#include <stdlib.h>
#include <string.h>

/* ── Constants ────────────────────────────────────────────── */

#define IOCP_POOL_QUEUE_CAPACITY 4096  /* must be power of 2 */
#define IOCP_POOL_MAX_WORKERS    4
#define IOCP_POOL_DRAIN_BATCH    256

/* ── Pool structure ───────────────────────────────────────── */

struct iocp_pool_s {
  coro_context_t *ctx;
  HANDLE          completion_port;

  /* Worker threads */
  HANDLE         *workers;
  int             worker_count;
  volatile LONG   stopping;

  /* Disruptor: workers → loop thread */
  disruptor_t          *queue;
  disruptor_consumer_t  consumer;
  uint64_t              consumer_seq;  /* next sequence to read */

  /* Tick coalescing — avoids flooding coro_post */
  volatile LONG   tick_posted;

  /* Global inflight tracking across all registered sockets */
  volatile LONG   inflight_count;
};

/* ── Forward declarations ─────────────────────────────────── */

static DWORD WINAPI iocp_pool_worker(LPVOID arg);
static void iocp_pool_tick(void *arg1, void *arg2);

/* ── Lifecycle ────────────────────────────────────────────── */

iocp_pool_t *iocp_pool_create(coro_context_t *ctx, int num_workers) {
  iocp_pool_t *pool;
  disruptor_config_t dcfg;
  int i;

  if (!ctx) return NULL;

  if (num_workers <= 0) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    num_workers = (int)si.dwNumberOfProcessors;
    if (num_workers > IOCP_POOL_MAX_WORKERS) num_workers = IOCP_POOL_MAX_WORKERS;
    if (num_workers < 1) num_workers = 1;
  }
  if (num_workers > IOCP_POOL_MAX_WORKERS) num_workers = IOCP_POOL_MAX_WORKERS;

  pool = (iocp_pool_t *)calloc(1, sizeof(*pool));
  if (!pool) return NULL;

  pool->ctx = ctx;

  /* Create shared IOCP port — concurrency hint = num_workers */
  pool->completion_port = CreateIoCompletionPort(
      INVALID_HANDLE_VALUE, NULL, 0, (DWORD)num_workers);
  if (!pool->completion_port) {
    free(pool);
    return NULL;
  }

  /* Create disruptor: entry_size = sizeof(iocp_op_t*), 1 consumer (loop thread) */
  dcfg.entry_size = sizeof(iocp_op_t *);
  dcfg.capacity = IOCP_POOL_QUEUE_CAPACITY;
  dcfg.consumer_capacity = 1;
  pool->queue = disruptor_create(&dcfg);
  if (!pool->queue) {
    CloseHandle(pool->completion_port);
    free(pool);
    return NULL;
  }

  /* Register the single consumer (loop thread) */
  pool->consumer_seq = disruptor_consumer_register(pool->queue, &pool->consumer);

  /* Spawn worker threads */
  pool->workers = (HANDLE *)calloc((size_t)num_workers, sizeof(HANDLE));
  if (!pool->workers) {
    disruptor_consumer_unregister(pool->queue, &pool->consumer);
    disruptor_destroy(pool->queue);
    CloseHandle(pool->completion_port);
    free(pool);
    return NULL;
  }

  pool->worker_count = num_workers;

  for (i = 0; i < num_workers; i++) {
    pool->workers[i] = CreateThread(NULL, 0, iocp_pool_worker, pool, 0, NULL);
    if (!pool->workers[i]) {
      pool->worker_count = i;
      iocp_pool_destroy(pool);
      return NULL;
    }
  }

  TLOG_DEBUG("iocp_pool: created with {:d} workers", num_workers);
  return pool;
}

void iocp_pool_destroy(iocp_pool_t *pool) {
  int i;

  if (!pool) return;

  InterlockedExchange(&pool->stopping, 1);

  /* Signal all workers to exit */
  for (i = 0; i < pool->worker_count; i++) {
    PostQueuedCompletionStatus(pool->completion_port, 0, 0, NULL);
  }

  /* Join all workers */
  for (i = 0; i < pool->worker_count; i++) {
    if (pool->workers[i]) {
      WaitForSingleObject(pool->workers[i], 3000);
      CloseHandle(pool->workers[i]);
    }
  }

  /* Drain remaining completions */
  iocp_pool_drain(pool);

  if (pool->queue) {
    disruptor_consumer_unregister(pool->queue, &pool->consumer);
    disruptor_destroy(pool->queue);
  }

  if (pool->completion_port) {
    CloseHandle(pool->completion_port);
  }

  free(pool->workers);
  free(pool);
}

/* ── Socket registration ──────────────────────────────────── */

int iocp_pool_associate(iocp_pool_t *pool, SOCKET socket) {
  HANDLE h;

  if (!pool || socket == INVALID_SOCKET) return TURBO_EINVAL;

  /* Key = 0 because the op itself carries the owner. We just need the
     socket's completions routed to our shared port. */
  h = CreateIoCompletionPort((HANDLE)socket, pool->completion_port, 0, 0);
  if (!h) {
    return -(int)GetLastError();
  }

  return 0;
}

HANDLE iocp_pool_get_port(iocp_pool_t *pool) {
  return pool ? pool->completion_port : NULL;
}

coro_context_t *iocp_pool_get_ctx(iocp_pool_t *pool) {
  return pool ? pool->ctx : NULL;
}

/* ── Worker thread ────────────────────────────────────────── */

static void iocp_pool_schedule_tick(iocp_pool_t *pool) {
  int rc;

  if (InterlockedExchange(&pool->tick_posted, 1) != 0) {
    return; /* tick already scheduled */
  }

  for (;;) {
    rc = coro_post(pool->ctx, iocp_pool_tick, pool, NULL);
    if (rc == 0) return;

    turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(pool->ctx));
    YieldProcessor();
  }
}

static DWORD WINAPI iocp_pool_worker(LPVOID arg) {
  iocp_pool_t *pool = (iocp_pool_t *)arg;
  DWORD bytes;
  ULONG_PTR key;
  OVERLAPPED *ov;

  while (1) {
    BOOL ok = GetQueuedCompletionStatus(
        pool->completion_port, &bytes, &key, &ov, INFINITE);

    if (!ov) {
      break; /* sentinel: time to exit */
    }

    iocp_op_t *op = CONTAINING_RECORD(ov, iocp_op_t, overlapped);
    op->bytes_transferred = bytes;
    op->status = ok ? 0 : -(int)GetLastError();

    /* Publish to disruptor (blocking claim — will spin until slot available) */
    {
      disruptor_cursor_t cursor;
      iocp_op_t **entry;

      disruptor_publisher_next_entry_blocking(pool->queue, &cursor);
      entry = (iocp_op_t **)disruptor_acquire_entry(pool->queue, &cursor);
      *entry = op;
      disruptor_publisher_publish(pool->queue, &cursor);
    }

    /* Coalesce wakeups: schedule a single tick on the loop thread */
    iocp_pool_schedule_tick(pool);
  }
  return 0;
}

/* ── Loop-thread drain ────────────────────────────────────── */

int iocp_pool_drain(iocp_pool_t *pool) {
  int processed = 0;
  disruptor_cursor_t cursor;

  if (!pool || !pool->queue) return 0;

  cursor.sequence = pool->consumer_seq;

  while (disruptor_consumer_wait_for_nonblocking(pool->queue, &cursor)) {
    /* cursor.sequence now holds the latest available sequence */
    uint64_t seq;
    for (seq = pool->consumer_seq; seq <= cursor.sequence; seq++) {
      disruptor_cursor_t entry_cursor;
      iocp_op_t **entry_ptr;
      iocp_op_t *op;

      entry_cursor.sequence = seq;
      entry_ptr = (iocp_op_t **)disruptor_acquire_entry(pool->queue, &entry_cursor);
      op = *entry_ptr;

      /* Dispatch to the registered handler */
      iocp_pool_dispatch_op(op);
      processed++;
    }

    /* Release consumed range */
    {
      disruptor_cursor_t release_cursor;
      release_cursor.sequence = cursor.sequence;
      disruptor_consumer_release_entry(pool->queue, &pool->consumer, &release_cursor);
    }

    pool->consumer_seq = cursor.sequence + 1;
    cursor.sequence = pool->consumer_seq;
  }

  return processed;
}

int iocp_pool_has_pending(iocp_pool_t *pool) {
  disruptor_cursor_t cursor;

  if (!pool || !pool->queue) return 0;

  cursor.sequence = pool->consumer_seq;
  return disruptor_consumer_wait_for_nonblocking(pool->queue, &cursor);
}

/* ── Tick callback (invoked via coro_post on loop thread) ── */

static void iocp_pool_tick(void *arg1, void *arg2) {
  iocp_pool_t *pool = (iocp_pool_t *)arg1;
  (void)arg2;

  InterlockedExchange(&pool->tick_posted, 0);
  iocp_pool_drain(pool);

  /* If more items arrived while we were draining, re-schedule */
  if (iocp_pool_has_pending(pool)) {
    iocp_pool_schedule_tick(pool);
  }
}

/* ── Inflight tracking ────────────────────────────────────── */

void iocp_pool_inflight_inc(iocp_pool_t *pool) {
  if (pool) InterlockedIncrement(&pool->inflight_count);
}

void iocp_pool_inflight_dec(iocp_pool_t *pool) {
  if (pool) InterlockedDecrement(&pool->inflight_count);
}

/* ── Dispatch: route completion to stream/datagram handler ── */

extern void stream_iocp_handle_connect_op(iocp_op_t *op);
extern void stream_iocp_handle_send_op(iocp_op_t *op);
extern void stream_iocp_handle_recv_op(iocp_op_t *op);
extern void stream_iocp_handle_accept_op(iocp_op_t *op);

void iocp_pool_dispatch_op(iocp_op_t *op) {
  if (!op) return;

  switch (op->kind) {
  case IOCP_OP_STREAM_CONNECT:
    stream_iocp_handle_connect_op(op);
    break;
  case IOCP_OP_STREAM_SEND:
    stream_iocp_handle_send_op(op);
    break;
  case IOCP_OP_STREAM_RECV:
    stream_iocp_handle_recv_op(op);
    break;
  case IOCP_OP_STREAM_ACCEPT:
    stream_iocp_handle_accept_op(op);
    break;

  case IOCP_OP_DG_SEND:
    extern void datagram_iocp_handle_send_op(iocp_op_t *op);
    datagram_iocp_handle_send_op(op);
    break;
  case IOCP_OP_DG_RECV:
    extern void datagram_iocp_handle_recv_op(iocp_op_t *op);
    datagram_iocp_handle_recv_op(op);
    break;

  default:
    free(op);
    break;
  }
}

#endif /* _WIN32 */
