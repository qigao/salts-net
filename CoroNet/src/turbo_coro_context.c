/**
 * @file coro_context.c
 * @brief Implementation of the opaque event-loop context.
 *
 * Uses turbo_loop_t (native IOCP/epoll/kqueue) instead of libuv.
 */

#include "turbo_coro_context.h"
#include "turbo_build_config_internal.h"
#include "CoroNet/turbo_coro_pool.h"
#include "platform.h"
#include "tlog.h"
#include "turbo_coro_internal.h"
#include "turbo_thread.h"
#include <stdlib.h>
#ifdef _WIN32
#include "turbo_iocp_pool.h"
#endif
#ifdef _WIN32
  #include <windows.h>
#else
  #include <errno.h>
  #include <sched.h>
#endif 
#ifdef _WIN32
static __declspec(thread) coro_context_t *tls_current_context = NULL;
#else
static __thread coro_context_t *tls_current_context = NULL;
#endif

int turbo_tcp_backend_is_available(int backend);
int turbo_udp_backend_is_available(int backend);

static turbo_tcp_backend_t turbo_tcp_backend_default(void) {
#ifdef _WIN32
  return TURBO_TCP_BACKEND_IOCP;
#elif defined(__linux__) || defined(__ANDROID__)
  return TURBO_TCP_BACKEND_EPOLL;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  return TURBO_TCP_BACKEND_KQUEUE;
#else
  return (turbo_tcp_backend_t)0;
#endif
}

/* ── coro_task_s definition ───────────────────────────────────── */
struct coro_task_s {
  coro_context_t *ctx;
  coro_fn         fn;
  void           *arg;
  int             started;
  int             cancelled;
  int             done;
  int             ref_count;
};


void *coro_context_native_loop(const coro_context_t *ctx) { return ctx ? (void *)ctx->loop : NULL; }

/* ── Forward declarations ───────────────────────────────────── */
static void cleanup_done_tasks(coro_context_t *ctx);
static void remove_task_from_context(coro_context_t *ctx, coro_task_t *task);
static int register_task_with_context(coro_context_t *ctx, coro_task_t *task);
static void destroy_task_now(coro_task_t *task);
static void pooled_coro_cleanup_callback(coro_t *co, void *arg);
static int ensure_post_queue(coro_context_t *ctx);
static void drain_post_queue(coro_context_t *ctx);
static int post_queue_empty(const coro_context_t *ctx);
static void drain_shutdown_callbacks(coro_context_t *ctx);
void coro_context_acquire_external(coro_context_t *ctx);
void coro_context_release_external(coro_context_t *ctx);
static int context_loop_alive(const coro_context_t *ctx);

static void coro_context_cleanup_create_failure(coro_context_t *ctx) {
  if (!ctx) {
    return;
  }

  if (ctx->scheduler) {
    coro_scheduler_destroy(ctx->scheduler);
    ctx->scheduler = NULL;
  }

  if (ctx->pool) {
    coro_object_pool_destroy(ctx->pool);
    ctx->pool = NULL;
  }

  if (ctx->tasks) {
    free(ctx->tasks);
    ctx->tasks = NULL;
  }

  if (ctx->post_initialized && ctx->post_ring) {
    free(ctx->post_ring);
    ctx->post_ring = NULL;
    ctx->post_initialized = 0;
  }

  if (ctx->arena && ctx->owns_arena) {
    mem_destroy(ctx->arena);
    free(ctx->arena);
    ctx->arena = NULL;
  }

  if (ctx->owns_loop && ctx->loop) {
    turbo_loop_destroy(ctx->loop);
    ctx->loop = NULL;
  }

  free(ctx);
}

coro_context_t *coro_context_create(void *loop) {
  coro_context_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  if (loop) {
    ctx->loop = (turbo_loop_t *)loop;
    ctx->owns_loop = 0;
  } else {
    ctx->loop = turbo_loop_create();
    if (!ctx->loop) {
      free(ctx);
      return NULL;
    }
    ctx->owns_loop = 1;
  }

  /* Initialize internal memory arena - each context has its own isolated pool */
  ctx->arena = (mem_pool_t *)calloc(1, sizeof(mem_pool_t));
  if (!ctx->arena) {
    coro_context_cleanup_create_failure(ctx);
    return NULL;
  }
  /* Pre-allocate 128KB to reduce fragmentation for typical workloads */
  if (mem_init(ctx->arena, MEM_ARENA_CONTEXT_INIT_SIZE) != 0) {
    coro_context_cleanup_create_failure(ctx);
    return NULL;
  }
  ctx->owns_arena = 1;

  /* Initialize lazy task list */
  ctx->task_capacity = 8;
  ctx->tasks = (coro_task_t **)calloc(ctx->task_capacity, sizeof(coro_task_t *));
  if (!ctx->tasks) {
    coro_context_cleanup_create_failure(ctx);
    return NULL;
  }
  ctx->task_count = 0;

  /* Initialize scheduler for managed coroutines */
  ctx->scheduler = coro_scheduler_create();
  if (!ctx->scheduler) {
    coro_context_cleanup_create_failure(ctx);
    return NULL;
  }

  /* Create coroutine pool with context's arena */
  ctx->pool = coro_object_pool_create(NULL, ctx);
  if (!ctx->pool) {
    coro_context_cleanup_create_failure(ctx);
    return NULL;
  }

  /* Initialize post queue early to avoid race conditions when posting from other threads. */
  if (ensure_post_queue(ctx) != 0) {
    coro_context_cleanup_create_failure(ctx);
    return NULL;
  }

  ctx->tcp_backend = turbo_tcp_backend_default();
  ctx->udp_backend = TURBO_UDP_BACKEND_AUTO;

  return ctx;
}

int coro_context_run(coro_context_t *ctx, turbo_run_mode_t mode) {
  if (!ctx || !ctx->loop) return 0;

  coro_context_t *prev = tls_current_context;
  tls_current_context = ctx;

  /* stop_requested is a one-shot signal for the current run() call.
   * If we keep it sticky, the next sync API invocation can return
   * immediately without actually draining its work. */
  ctx->stop_requested = 0;

  int r = 0;

  if (mode == TURBO_RUN_DEFAULT) {
    while (1) {
      int has_ready = coro_scheduler_has_ready(ctx->scheduler);

      if (ctx->stop_requested ||
          (!context_loop_alive(ctx) &&
           !coro_scheduler_count(ctx->scheduler) &&
           atomic_load_explicit(&ctx->external_refs, memory_order_acquire) == 0 &&
           post_queue_empty(ctx))) {
        break;
      }

      /* If we have ready coros, don't block. If not, wait for wake. */
      if (has_ready) {
        turbo_loop_poll(ctx->loop, 2, 0); /* NOWAIT */
      } else {
        turbo_loop_poll(ctx->loop, -1, 1); /* block until wake */
      }

      drain_post_queue(ctx);

      if (ctx->scheduler) {
        coro_scheduler_tick(ctx->scheduler);
      }
      cleanup_done_tasks(ctx);

      if (ctx->stop_requested ||
          (!context_loop_alive(ctx) &&
           !coro_scheduler_count(ctx->scheduler) &&
           atomic_load_explicit(&ctx->external_refs, memory_order_acquire) == 0 &&
           post_queue_empty(ctx))) {
        break;
      }
    }
  } else {
    /* Manual/Once mode.
     * On Windows turbo_loop_poll(block=1) is Sleep(N), which adds N ms of
     * latency per yield point even when IOCP completions are already queued
     * in the post_queue (they arrive via coro_post from worker threads, not
     * from the poll iteration itself).  Only sleep when there is genuinely
     * nothing pending. */
    int has_ready = coro_scheduler_has_ready(ctx->scheduler);
    if (mode == TURBO_RUN_NOWAIT || has_ready || !post_queue_empty(ctx)) {
      turbo_loop_poll(ctx->loop, 2, 0); /* NOWAIT */
    } else {
      turbo_loop_poll(ctx->loop, 1, 1); /* idle ONCE: wait briefly for posted I/O */
    }

    drain_post_queue(ctx);

    if (ctx->scheduler) {
      coro_scheduler_tick(ctx->scheduler);
    }
    cleanup_done_tasks(ctx);
  }

  tls_current_context = prev;
  return r;
}

void coro_context_stop(coro_context_t *ctx) {
  if (!ctx) return;
  ctx->stop_requested = 1;
  turbo_loop_stop(ctx->loop);
}

int coro_context_alive(coro_context_t *ctx) {
  if (!ctx) return 0;
  if (atomic_load_explicit(&ctx->external_refs, memory_order_acquire) != 0) return 1;
  if (context_loop_alive(ctx)) return 1;
  if (ctx->scheduler && coro_scheduler_count(ctx->scheduler) > 0) return 1;
  if (ctx->post_initialized) {
    int tail = atomic_load_explicit(&ctx->post_tail, memory_order_relaxed);
    int head = atomic_load_explicit(&ctx->post_head, memory_order_relaxed);
    if (tail != head) return 1;
  }
  return 0;
}

int coro_context_coro_count(coro_context_t *ctx) {
  if (!ctx || !ctx->scheduler) return 0;
  return coro_scheduler_count(ctx->scheduler);
}

uint64_t coro_context_now(coro_context_t *ctx) {
  if (!ctx) return 0;
  return turbo_loop_now(ctx->loop);
}

static int post_queue_empty(const coro_context_t *ctx) {
  int tail;
  int head;

  if (!ctx || !ctx->post_initialized) return 1;

  tail = atomic_load_explicit((atomic_int *)&ctx->post_tail, memory_order_relaxed);
  head = atomic_load_explicit((atomic_int *)&ctx->post_head, memory_order_relaxed);
  return tail == head;
}

static int context_loop_alive(const coro_context_t *ctx) {
  if (!ctx || !ctx->loop || !ctx->owns_loop) {
    return 0;
  }
  return turbo_loop_alive(ctx->loop);
}

static void drain_shutdown_callbacks(coro_context_t *ctx) {
  int forced_teardown;
  coro_context_t *prev;
  uint64_t deadline_ms;

  if (!ctx || !ctx->loop || !ctx->post_initialized) return;

  prev = tls_current_context;
  tls_current_context = ctx;
  forced_teardown = 1;
  deadline_ms = turbo_uptime_ms() + 8000;

  while (turbo_uptime_ms() < deadline_ms) {
    int waiting_for_shutdown;

    waiting_for_shutdown =
        atomic_load_explicit(&ctx->external_refs, memory_order_acquire) != 0 ||
        context_loop_alive(ctx) ||
        (ctx->scheduler && coro_scheduler_count(ctx->scheduler) > 0) ||
        !post_queue_empty(ctx);

    if (waiting_for_shutdown) {
#ifdef _WIN32
      turbo_loop_poll(ctx->loop, 0, 0);
      turbo_thread_yield();
#else
      turbo_loop_poll(ctx->loop, 1, 1);
#endif
    } else {
      turbo_loop_poll(ctx->loop, 2, 0);
    }
    drain_post_queue(ctx);

    if (ctx->scheduler) {
      coro_scheduler_tick(ctx->scheduler);
    }
    cleanup_done_tasks(ctx);

    if (atomic_load_explicit(&ctx->external_refs, memory_order_acquire) == 0 &&
        !context_loop_alive(ctx) &&
        (!ctx->scheduler || coro_scheduler_count(ctx->scheduler) == 0) &&
        post_queue_empty(ctx)) {
      forced_teardown = 0;
      break;
    }
  }

  tls_current_context = prev;

  if (forced_teardown &&
      (atomic_load_explicit(&ctx->external_refs, memory_order_acquire) != 0 ||
       context_loop_alive(ctx) ||
       (ctx->scheduler && coro_scheduler_count(ctx->scheduler) > 0) ||
       !post_queue_empty(ctx))) {
    TLOG_WARN(
        "coro_context_destroy: forced teardown with pending loop work "
        "(ctx={}, external_refs={}, loop_alive={}, coro_count={}, post_empty={})",
        (void *)ctx, atomic_load_explicit(&ctx->external_refs, memory_order_acquire),
        turbo_loop_alive(ctx->loop), ctx->scheduler ? coro_scheduler_count(ctx->scheduler) : 0,
        post_queue_empty(ctx));
  }
}

void coro_context_acquire_external(coro_context_t *ctx) {
  if (!ctx) return;
  atomic_fetch_add_explicit(&ctx->external_refs, 1, memory_order_acq_rel);
}

void coro_context_release_external(coro_context_t *ctx) {
  if (!ctx) return;
  atomic_fetch_sub_explicit(&ctx->external_refs, 1, memory_order_acq_rel);
}

void coro_context_destroy(coro_context_t *ctx) {
  if (!ctx) return;

  /*
   * Transport shutdown is asynchronous on some backends. Drain posted close
   * callbacks before freeing the context so worker threads do not post into
   * freed memory.
   */
  drain_shutdown_callbacks(ctx);

#ifdef _WIN32
  /* Destroy shared IOCP pool before scheduler teardown so worker threads
   * exit cleanly before coroutine state is freed. */
  if (ctx->iocp_pool) {
    iocp_pool_destroy(ctx->iocp_pool);
    ctx->iocp_pool = NULL;
  }
#endif

  if (atomic_load_explicit(&ctx->external_refs, memory_order_acquire) != 0) {
    TLOG_WARN("coro_context_destroy: abandoning context teardown while external refs remain");
    return;
  }

  /* Destroy scheduler (handles all spawned coroutines) */
  if (ctx->scheduler) {
    coro_scheduler_destroy(ctx->scheduler);
    ctx->scheduler = NULL;
  }

  /* Destroy pool */
  if (ctx->pool) {
    if (coro_object_pool_active_count(ctx->pool) > 0) {
      TLOG_WARN(
          "coro_context_destroy: dropping {} stale pooled coroutines after scheduler teardown",
          coro_object_pool_active_count(ctx->pool));
      coro_object_pool_forget_active(ctx->pool);
    }
    coro_object_pool_destroy(ctx->pool);
    ctx->pool = NULL;
  }

  /* Free remaining tasks owned by this context. */
  for (int i = 0; i < ctx->task_count; i++) {
    destroy_task_now(ctx->tasks[i]);
  }

  /* Free tasks array */
  if (ctx->tasks) {
    free(ctx->tasks);
    ctx->tasks = NULL;
  }
  ctx->task_count = 0;
  ctx->task_capacity = 0;

  /* Cleanup arena: destroy and free since each context owns its arena */
  if (ctx->arena && ctx->owns_arena) {
    mem_destroy(ctx->arena);
    free(ctx->arena);
    ctx->arena = NULL;
  }

  /* Free ring buffer */
  if (ctx->post_initialized) {
    if (ctx->post_ring) {
      free(ctx->post_ring);
      ctx->post_ring = NULL;
    }
    ctx->post_initialized = 0;
  }

  if (ctx->owns_loop && ctx->loop) {
    turbo_loop_destroy(ctx->loop);
    ctx->loop = NULL;
  }
  free(ctx);
}

/* ── Post queue: thread-safe callback posting to event loop ── */

/**
 * @brief Drain all pending tasks from the lock-free ring buffer.
 * Good taste: No malloc, no mutex, just atomic reads.
 */
static void drain_post_queue(coro_context_t *ctx) {
  if (!ctx->post_initialized) return;

  int head = atomic_load_explicit(&ctx->post_head, memory_order_acquire);

  while (1) {
    int tail = atomic_load_explicit(&ctx->post_tail, memory_order_relaxed);

    if (tail == head) break;

    coro_post_slot_t slot = ctx->post_ring[tail];
    atomic_store_explicit(&ctx->post_tail, (tail + 1) & (ctx->post_ring_size - 1),
                          memory_order_release);

    slot.fn(slot.arg1, slot.arg2);
  }
}

/**
 * @brief Initialize the lock-free post queue.
 */
static int ensure_post_queue(coro_context_t *ctx) {
  if (ctx->post_initialized) return 0;

  /* Default ring size: 16384 slots (can handle burst of 16383 concurrent posts) */
  ctx->post_ring_size = 16384;
  ctx->post_ring = (coro_post_slot_t *)calloc(ctx->post_ring_size, sizeof(coro_post_slot_t));
  if (!ctx->post_ring) return TURBO_ENOMEM;

  atomic_store_explicit(&ctx->post_head, 0, memory_order_relaxed);
  atomic_store_explicit(&ctx->post_tail, 0, memory_order_relaxed);
  atomic_store_explicit(&ctx->post_lock, 0, memory_order_relaxed);

  ctx->post_initialized = 1;
  return 0;
}

/**
 * @brief Post a callback to be executed on the context's event loop thread.
 *
 * Thread-safe, lock-free implementation using ring buffer + atomic operations.
 */
int coro_post(coro_context_t *ctx, coro_post_fn fn, void *arg1, void *arg2) {
  if (!ctx || !fn) return TURBO_EINVAL;

  if (!ctx->post_initialized) return TURBO_EINVAL;

  /* Good taste: Check capacity BEFORE acquiring lock (optimistic fast path). */
  int head = atomic_load_explicit(&ctx->post_head, memory_order_acquire);
  int tail = atomic_load_explicit(&ctx->post_tail, memory_order_acquire);
  int next_head = (head + 1) & (ctx->post_ring_size - 1);

  if (next_head == tail) {
    return TURBO_ENOMEM;
  }

  /* Lock the producer side */
  while (atomic_exchange_explicit(&ctx->post_lock, 1, memory_order_acquire)) {
#ifdef _WIN32
    YieldProcessor();
#else
    __asm__ volatile("pause" ::: "memory");
#endif
  }

  /* Re-check after acquiring lock (TOCTOU protection) */
  head = atomic_load_explicit(&ctx->post_head, memory_order_relaxed);
  tail = atomic_load_explicit(&ctx->post_tail, memory_order_acquire);
  next_head = (head + 1) & (ctx->post_ring_size - 1);

  if (next_head == tail) {
    atomic_store_explicit(&ctx->post_lock, 0, memory_order_release);
    return TURBO_ENOMEM;
  }

  ctx->post_ring[head].fn = fn;
  ctx->post_ring[head].arg1 = arg1;
  ctx->post_ring[head].arg2 = arg2;

  atomic_store_explicit(&ctx->post_head, next_head, memory_order_release);
  atomic_store_explicit(&ctx->post_lock, 0, memory_order_release);

  turbo_loop_wake(ctx->loop);
  return 0;
}

void coro_context_set_persistent(coro_context_t *ctx, int persistent) {
  if (!ctx) return;

  if (persistent && !ctx->persistent) {
    turbo_loop_ref(ctx->loop);
    ctx->persistent = 1;
  } else if (!persistent && ctx->persistent) {
    turbo_loop_unref(ctx->loop);
    ctx->persistent = 0;
  }
}

int coro_context_set_tcp_backend(coro_context_t *ctx, turbo_tcp_backend_t backend) {
  if (!ctx) {
    return TURBO_EINVAL;
  }

  if (!turbo_tcp_backend_is_available(backend)) {
    return TURBO_EPROTONOSUPPORT;
  }

  ctx->tcp_backend = backend;
  return 0;
}

turbo_tcp_backend_t coro_context_get_tcp_backend(const coro_context_t *ctx) {
  return ctx ? ctx->tcp_backend : turbo_tcp_backend_default();
}

int coro_context_set_udp_backend(coro_context_t *ctx, turbo_udp_backend_t backend) {
  if (!ctx) {
    return TURBO_EINVAL;
  }

  if (!turbo_udp_backend_is_available(backend)) {
    return TURBO_EPROTONOSUPPORT;
  }

  ctx->udp_backend = backend;
  return 0;
}

turbo_udp_backend_t coro_context_get_udp_backend(const coro_context_t *ctx) {
  return ctx ? ctx->udp_backend : TURBO_UDP_BACKEND_AUTO;
}

int coro_context_get_last_error(const coro_context_t *ctx) {
  return ctx ? ctx->last_error : 0;
}

/* ── Keepalive ref management ─────────────────────────────────── */

void coro_context_native_ref(coro_context_t *ctx) {
  if (!ctx) return;
  ctx->native_keepalive_refs++;
  turbo_loop_ref(ctx->loop);
}

void coro_context_native_unref(coro_context_t *ctx) {
  if (!ctx || ctx->native_keepalive_refs <= 0) return;
  ctx->native_keepalive_refs--;
  turbo_loop_unref(ctx->loop);
}

/* ── Coroutine Spawning ───────────────────────────────────────── */

int coro_context_spawn(coro_context_t *ctx, coro_fn fn, void *arg) {
  if (!ctx || !fn) return TURBO_EINVAL;

  coro_t *co = coro_object_pool_acquire(ctx->pool, fn, arg);
  if (!co) return TURBO_ENOMEM;

  coro_set_cleanup(co, pooled_coro_cleanup_callback, ctx->pool);
  coro_scheduler_adopt(ctx->scheduler, co);
  turbo_loop_wake(ctx->loop);
  return 0;
}

/* ── Task Management ──────────────────────────────────────────── */

static void remove_task_from_context(coro_context_t *ctx, coro_task_t *task) {
  for (int i = 0; i < ctx->task_count; i++) {
    if (ctx->tasks[i] == task) {
      ctx->tasks[i] = ctx->tasks[ctx->task_count - 1];
      ctx->task_count--;
      return;
    }
  }
}

static int register_task_with_context(coro_context_t *ctx, coro_task_t *task) {
  if (!ctx || !task) return TURBO_EINVAL;

  if (ctx->task_count >= ctx->task_capacity) {
    int new_cap = ctx->task_capacity * 2;
    coro_task_t **new_tasks = realloc(ctx->tasks, new_cap * sizeof(coro_task_t *));
    if (!new_tasks) {
      return TURBO_ENOMEM;
    }
    ctx->tasks = new_tasks;
    ctx->task_capacity = new_cap;
  }

  ctx->tasks[ctx->task_count++] = task;
  return 0;
}

static void cleanup_done_tasks(coro_context_t *ctx) {
  for (int i = ctx->task_count - 1; i >= 0; i--) {
    coro_task_t *task = ctx->tasks[i];
    if (coro_task_is_done(task) && task->ref_count == 0) {
      remove_task_from_context(ctx, task);
      destroy_task_now(task);
    }
  }
}

/* ── Task API ─────────────────────────────────────────────────── */

static void task_coro_wrapper(coro_t *co, void *arg) {
  (void)co;
  coro_task_t *task = (coro_task_t *)arg;
  task->fn(co, task->arg);
  task->done = 1;
}

coro_task_t *coro_task_create(coro_context_t *ctx, coro_fn fn, void *arg) {
  if (!ctx || !fn) return NULL;

  coro_task_t *task = (coro_task_t *)malloc(sizeof(coro_task_t));
  if (!task) return NULL;
  task->ctx       = ctx;
  task->fn        = fn;
  task->arg       = arg;
  task->started   = 0;
  task->cancelled = 0;
  task->done      = 0;
  task->ref_count = 1;

  if (register_task_with_context(ctx, task) != 0) {
    free(task);
    return NULL;
  }

  return task;
}

static void pooled_coro_cleanup_callback(coro_t *co, void *arg) {
  coro_object_pool_t *pool = (coro_object_pool_t *)arg;
  coro_object_pool_release(pool, co);
}

int coro_task_start(coro_task_t *task) {
  if (!task || !task->ctx || !task->fn) return TURBO_EINVAL;
  if (task->started || task->cancelled || task->done) return TURBO_EINVAL;

  task->started = 1;

  coro_t *co = coro_object_pool_acquire(task->ctx->pool, task_coro_wrapper, task);
  if (!co) {
    task->started = 0;
    return TURBO_ENOMEM;
  }
  
  /* Return pooled coroutine shell after task completion. */
  coro_set_cleanup(co, pooled_coro_cleanup_callback, task->ctx->pool);
  
  coro_scheduler_adopt(task->ctx->scheduler, co);
  return 0;
}

int coro_task_cancel(coro_task_t *task) {
  if (!task) return TURBO_EINVAL;
  if (task->started || task->done) return TURBO_EINVAL;

  task->cancelled = 1;
  task->done = 1;
  return 0;
}

int coro_task_is_done(coro_task_t *task) {
  return task ? task->done : 1;
}

static void destroy_task_now(coro_task_t *task) {
  free(task);
}

void coro_task_destroy(coro_task_t *task) {
  if (!task) return;

  if (task->ref_count > 0) {
    task->ref_count--;
  }

  if (task->ref_count == 0 && task->ctx) {
    remove_task_from_context(task->ctx, task);
  }

  if (task->ref_count == 0 && (task->done || !task->started)) {
    destroy_task_now(task);
    return;
  }
}

coro_task_t *coro_context_spawn_task(coro_context_t *ctx, coro_fn fn, void *arg) {
  return coro_task_create(ctx, fn, arg);
}

/* ── when_all / when_any ──────────────────────────────────────── */

int coro_when_all(coro_context_t *ctx, coro_task_t **tasks, int count) {
  (void)ctx;
  if (!tasks || count <= 0) return TURBO_EINVAL;

  ASSERT_IN_CORO();

  /* Bump ref counts so tasks aren't cleaned up while we wait */
  for (int i = 0; i < count; i++) {
    if (!tasks[i]) return TURBO_EINVAL;
    tasks[i]->ref_count++;
  }

  /* Spin-yield until all tasks are done */
  while (1) {
    int all_done = 1;
    for (int i = 0; i < count; i++) {
      if (!coro_task_is_done(tasks[i])) {
        all_done = 0;
        break;
      }
    }
    if (all_done) break;
    coro_yield();
  }

  /* Release references */
  for (int i = 0; i < count; i++) {
    tasks[i]->ref_count--;
  }
  return 0;
}

int coro_when_any(coro_context_t *ctx, coro_task_t **tasks, int count) {
  (void)ctx;
  if (!tasks || count <= 0) return TURBO_EINVAL;

  ASSERT_IN_CORO();

  for (int i = 0; i < count; i++) {
    if (!tasks[i]) return TURBO_EINVAL;
    tasks[i]->ref_count++;
  }

  int winner = -1;
  while (winner < 0) {
    for (int i = 0; i < count; i++) {
      if (coro_task_is_done(tasks[i])) {
        winner = i;
        break;
      }
    }
    if (winner >= 0) break;
    coro_yield();
  }

  /* Release references */
  for (int i = 0; i < count; i++) {
    tasks[i]->ref_count--;
  }

  return winner;
}

/* ── Coroutine Sleep ──────────────────────────────────────────── */

typedef struct {
  coro_t *co;
  int co_is_scheduled;
  coro_context_t *ctx;
  turbo_timer_t *timer;
} sleep_ctx_t;

static void on_sleep_timer_bounce(void *arg1, void *arg2) {
  (void)arg2;
  sleep_ctx_t *sctx = (sleep_ctx_t *)arg1;

  if (sctx->co_is_scheduled) {
    coro_set_waiting_for_io(sctx->co, 0);
  } else {
    coro_resume(sctx->co);
  }

  turbo_timer_destroy(sctx->timer);
  coro_context_release_external(sctx->ctx);
  free(sctx);
}

static void on_sleep_timer(turbo_timer_t *timer) {
  sleep_ctx_t *sctx = (sleep_ctx_t *)turbo_timer_get_data(timer);
  int rc;

  if (!sctx) {
    return;
  }

  rc = coro_post(sctx->ctx, on_sleep_timer_bounce, sctx, NULL);
  if (rc != 0) {
    on_sleep_timer_bounce(sctx, NULL);
  }
}

void coro_sleep(coro_context_t *ctx, uint64_t ms) {
  if (!ctx) return;

  ASSERT_IN_CORO();

  coro_t *co = coro_running();
  if (!co) return;

  /* 0ms = just yield to scheduler */
  if (ms == 0) {
    coro_yield();
    return;
  }

  sleep_ctx_t *sctx = malloc(sizeof(sleep_ctx_t));
  if (!sctx) return;

  sctx->co = co;
  sctx->co_is_scheduled = coro_is_scheduled(co);
  sctx->ctx = ctx;
  sctx->timer = turbo_timer_create(NULL);
  if (!sctx->timer) {
    free(sctx);
    return;
  }

  turbo_timer_set_data(sctx->timer, sctx);

  if (sctx->co_is_scheduled) {
    coro_set_waiting_for_io(co, 1);
  }

  coro_context_acquire_external(ctx);
  if (turbo_timer_start(sctx->timer, on_sleep_timer, ms, 0) != 0) {
    if (sctx->co_is_scheduled) {
      coro_set_waiting_for_io(co, 0);
    }
    coro_context_release_external(ctx);
    turbo_timer_destroy(sctx->timer);
    free(sctx);
    return;
  }
  coro_yield();
}

#ifndef _WIN32
  #include <poll.h>
  #include <unistd.h>
  #include <fcntl.h>
  #ifdef __linux__
    #include <sys/eventfd.h>
  #endif
#endif

/* ── Stubs for native loop methods ── */
struct turbo_loop_s {
  int ref_count;
#ifdef _WIN32
  HANDLE wake_event;
#else
  int wake_fds[2]; /* [0]=read, [1]=write (pipe) or wake_fds[0]=eventfd */
#endif
};

turbo_loop_t *turbo_loop_create(void) {
#ifdef _WIN32
  static int wsa_init = 0;
  if (!wsa_init) {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    wsa_init = 1;
  }
#endif

  turbo_loop_t *loop = (turbo_loop_t *)calloc(1, sizeof(turbo_loop_t));
  if (!loop) return NULL;

#ifdef _WIN32
  loop->wake_event = CreateEvent(NULL, FALSE, FALSE, NULL);
#else
#ifdef __linux__
  loop->wake_fds[0] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  loop->wake_fds[1] = -1;
#else
  if (pipe(loop->wake_fds) == 0) {
    int f1 = fcntl(loop->wake_fds[0], F_GETFL, 0);
    fcntl(loop->wake_fds[0], F_SETFL, f1 | O_NONBLOCK);
    int f2 = fcntl(loop->wake_fds[1], F_GETFL, 0);
    fcntl(loop->wake_fds[1], F_SETFL, f2 | O_NONBLOCK);
  }
#endif
#endif

  return loop;
}

void turbo_loop_destroy(turbo_loop_t *loop) {
  if (!loop) return;
#ifdef _WIN32
  if (loop->wake_event) CloseHandle(loop->wake_event);
#else
  if (loop->wake_fds[0] >= 0) close(loop->wake_fds[0]);
  if (loop->wake_fds[1] >= 0) close(loop->wake_fds[1]);
#endif
  free(loop);
}

void turbo_loop_stop(turbo_loop_t *loop) { (void)loop; }

void turbo_loop_poll(turbo_loop_t *loop, int max_ms, int block) {
  if (!loop) return;
#ifdef _WIN32
  if (block && max_ms != 0) {
    WaitForSingleObject(loop->wake_event, (max_ms < 0) ? INFINITE : (DWORD)max_ms);
  } else {
    /* Non-blocking: just check if signaled, don't wait. */
    WaitForSingleObject(loop->wake_event, 0);
  }
#else
  struct pollfd pfd;
  pfd.fd = loop->wake_fds[0];
  pfd.events = POLLIN;
  pfd.revents = 0;

  int timeout = block ? max_ms : 0;

  int r = poll(&pfd, 1, timeout);
  if (r > 0 && (pfd.revents & POLLIN)) {
    /* Clear wake signal */
    uint64_t val;
#ifdef __linux__
    read(loop->wake_fds[0], &val, sizeof(val));
#else
    while (read(loop->wake_fds[0], &val, sizeof(val)) > 0);
#endif
  }
#endif
}

int turbo_loop_alive(turbo_loop_t *loop) { return loop ? loop->ref_count > 0 : 0; }
uint64_t turbo_loop_now(turbo_loop_t *loop) {
  (void)loop;
#ifdef _WIN32
  return GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

void turbo_loop_ref(turbo_loop_t *loop) { if (loop) loop->ref_count++; }
void turbo_loop_unref(turbo_loop_t *loop) { if (loop && loop->ref_count > 0) loop->ref_count--; }

void turbo_loop_wake(turbo_loop_t *loop) {
  if (!loop) return;
#ifdef _WIN32
  if (loop->wake_event) SetEvent(loop->wake_event);
#else
  uint64_t val = 1;
#ifdef __linux__
  if (loop->wake_fds[0] >= 0) {
    ssize_t n;
    do {
      n = write(loop->wake_fds[0], &val, sizeof(val));
    } while (n < 0 && errno == EINTR);
  }
#else
  if (loop->wake_fds[1] >= 0) {
    ssize_t n;
    do {
      n = write(loop->wake_fds[1], &val, sizeof(val));
    } while (n < 0 && errno == EINTR);
  }
#endif
#endif
}

/* ── Context Accessors ── */
coro_context_t *coro_context_current(void) {
  return tls_current_context;
}
void* coro_get_memory_pool(void) {
  return tls_current_context ? tls_current_context->arena : NULL;
}
void* coro_context_get_arena(coro_context_t *ctx) {
  return ctx ? ctx->arena : NULL;
}

/* ── Backend info ── */
int turbo_tcp_backend_is_available(int backend) {
  switch (backend) {
#ifdef _WIN32
    case TURBO_TCP_BACKEND_IOCP:
      return 1;
#elif defined(__linux__) || defined(__ANDROID__)
#if !defined(__ANDROID__) && TURBO_HAS_IO_URING
    case TURBO_TCP_BACKEND_IO_URING:
      return 1;
#endif
    case TURBO_TCP_BACKEND_EPOLL:
      return 1;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    case TURBO_TCP_BACKEND_KQUEUE:
      return 1;
#endif
    default:
      return 0;
  }
}

int turbo_udp_backend_is_available(int backend) {
  switch (backend) {
    case TURBO_UDP_BACKEND_AUTO:
      return 1;
#ifdef _WIN32
    case TURBO_UDP_BACKEND_IOCP:
      return 1;
#elif defined(__linux__) && !defined(__ANDROID__) && TURBO_HAS_IO_URING
    case TURBO_UDP_BACKEND_IO_URING:
      return 1;
#elif defined(__linux__) || defined(__ANDROID__)
    case TURBO_UDP_BACKEND_EPOLL:
      return 1;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    case TURBO_UDP_BACKEND_KQUEUE:
      return 1;
#endif
    default:
      return 0;
  }
}
