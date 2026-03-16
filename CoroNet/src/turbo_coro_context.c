/**
 * @file coro_context.c
 * @brief Implementation of the opaque event-loop context.
 *
 */

#include "turbo_coro_context.h"
#include "turbo_coro_internal.h"
#include "CoroNet/turbo_coro_pool.h"
#include "turbo_thread.h"
#include <stdlib.h>
#include <uv.h>
#include "tlog.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

/* Compile-time guarantee: turbo error codes == libuv error codes.
   MSVC C11 mode uses _Static_assert; C23/C++ use static_assert. */
#ifndef __cplusplus
  #if defined(_MSC_VER) && !defined(_Static_assert)
    #define _Static_assert static_assert
  #endif
#endif

_Static_assert(TURBO_EOF == UV_EOF, "TURBO_EOF mismatch");
_Static_assert(TURBO_ENOMEM == UV_ENOMEM, "TURBO_ENOMEM mismatch");
_Static_assert(TURBO_EINVAL == UV_EINVAL, "TURBO_EINVAL mismatch");
_Static_assert(TURBO_ETIMEDOUT == UV_ETIMEDOUT, "TURBO_ETIMEDOUT mismatch");
_Static_assert(TURBO_ECONNREFUSED == UV_ECONNREFUSED, "TURBO_ECONNREFUSED mismatch");
_Static_assert(TURBO_EPROTONOSUPPORT == UV_EPROTONOSUPPORT, "TURBO_EPROTONOSUPPORT mismatch");
_Static_assert(TURBO_EALREADY == UV_EALREADY, "TURBO_EALREADY mismatch");

#ifdef _WIN32
static __declspec(thread) coro_context_t *tls_current_context = NULL;
#else
static __thread coro_context_t *tls_current_context = NULL;
#endif

coro_context_t *coro_context_current(void) { return tls_current_context; }

/* ── Forward declarations ───────────────────────────────────── */
static void cleanup_done_tasks(coro_context_t *ctx);
static void remove_task_from_context(coro_context_t *ctx, coro_task_t *task);
static int ensure_post_queue(coro_context_t *ctx);

coro_context_t *coro_context_create(void *loop) {
  coro_context_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  if (loop) {
    ctx->loop = (uv_loop_t *)loop;
    ctx->owns_loop = 0;
  } else {
    ctx->loop = malloc(sizeof(uv_loop_t));
    if (!ctx->loop) {
      free(ctx);
      return NULL;
    }
    uv_loop_init(ctx->loop);
    ctx->owns_loop = 1;
  }

  /* Initialize internal memory arena - each context has its own isolated pool */
  ctx->arena = (mem_pool_t*)calloc(1, sizeof(mem_pool_t));
  if (!ctx->arena) {
    if (ctx->owns_loop) {
      uv_loop_close(ctx->loop);
      free(ctx->loop);
    }
    free(ctx);
    return NULL;
  }
  /* Pre-allocate 128KB to reduce fragmentation for typical workloads */
  if (mem_init(ctx->arena, MEM_ARENA_CONTEXT_INIT_SIZE) != 0) {
    free(ctx->arena);
    if (ctx->owns_loop) {
      uv_loop_close(ctx->loop);
      free(ctx->loop);
    }
    free(ctx);
    return NULL;
  }
  ctx->owns_arena = 1;

  /* Initialize lazy task list */
  ctx->task_capacity = 8;
  ctx->tasks = (coro_task_t **)calloc(ctx->task_capacity, sizeof(coro_task_t *));
  if (!ctx->tasks) {
    if (ctx->owns_loop) {
      uv_loop_close(ctx->loop);
      free(ctx->loop);
    }
    
    free(ctx);
    return NULL;
  }
  ctx->task_count = 0;

  /* Initialize scheduler for managed coroutines */
  ctx->scheduler = coro_scheduler_create();
  if (!ctx->scheduler) {
    if (ctx->owns_loop) {
      uv_loop_close(ctx->loop);
      free(ctx->loop);
    }
    free(ctx);
    return NULL;
  }

  /* Create coroutine pool with context's arena */
  ctx->pool = coro_object_pool_create(NULL, ctx);

  /* Initialize post queue early to avoid race conditions when posting from other threads.
     The post_async handle must be initialized on the loop thread, but we expect
     coro_context_create to be called from the 'home' thread of this context. */
  if (ensure_post_queue(ctx) != 0) {
    coro_context_destroy(ctx);
    return NULL;
  }

  return ctx;
}

int coro_context_run(coro_context_t *ctx, turbo_run_mode_t mode) {
  if (!ctx || !ctx->loop) return 0;

  coro_context_t *prev = tls_current_context;
  tls_current_context = ctx;

  int r = 0;
  ctx->stop_requested = 0;

  if (mode == TURBO_RUN_DEFAULT) {
    while (1) {
      int has_ready = coro_scheduler_has_ready(ctx->scheduler);
      
      /* If we have ready coros, don't block. If not, wait for IO. */
      r = uv_run(ctx->loop, has_ready ? UV_RUN_NOWAIT : UV_RUN_ONCE);

      if (ctx->scheduler) {
        coro_scheduler_tick(ctx->scheduler);
      }
      cleanup_done_tasks(ctx);

      if (ctx->stop_requested || (!uv_loop_alive(ctx->loop) && !coro_scheduler_count(ctx->scheduler))) {
        break;
      }
    }
  } else {
    /* Manual/Once mode: also honor ready coros by not blocking if they exist */
    int has_ready = coro_scheduler_has_ready(ctx->scheduler);
    uv_run_mode uv_mode = (uv_run_mode)mode;
    if (has_ready && uv_mode == UV_RUN_ONCE) {
       uv_mode = UV_RUN_NOWAIT;
    }
    
    r = uv_run(ctx->loop, uv_mode);

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
  uv_stop(ctx->loop);
  
  /* Wake up the loop if it's blocked in another thread */
  if (ctx->post_initialized) {
    uv_async_send(&ctx->post_async);
  }
}

int coro_context_alive(coro_context_t *ctx) {
  if (!ctx) return 0;
  return uv_loop_alive(ctx->loop);
}

int coro_context_coro_count(coro_context_t *ctx) {
  if (!ctx || !ctx->scheduler) return 0;
  return coro_scheduler_count(ctx->scheduler);
}

uint64_t coro_context_now(coro_context_t *ctx) {
  if (!ctx) return 0;
  return uv_now(ctx->loop);
}

void coro_context_destroy(coro_context_t *ctx) {
  if (!ctx) return;

  /* Destroy scheduler (handles all spawned coroutines) */
  if (ctx->scheduler) {
    coro_scheduler_destroy(ctx->scheduler);
    ctx->scheduler = NULL;
  }

  /* Destroy pool */
  if (ctx->pool) {
    coro_object_pool_destroy(ctx->pool);
    ctx->pool = NULL;
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

  if (ctx->post_initialized) {
    uv_close((uv_handle_t *)&ctx->post_async, NULL);
    /* Free ring buffer (no nodes to drain, just the buffer itself) */
    if (ctx->post_ring) {
      free(ctx->post_ring);
      ctx->post_ring = NULL;
    }
    ctx->post_initialized = 0;
  }
  if (ctx->owns_loop && ctx->loop) {
    /* 1. Unref handles that we might have ref'd for persistence */
    if (ctx->post_initialized) {
      uv_unref((uv_handle_t *)&ctx->post_async);
    }

    /* 2. Drain pending close callbacks (e.g. post_async/timer closes)
       before closing the owned loop. We must use UV_RUN_NOWAIT or UV_RUN_ONCE
       to ensure we don't block forever if some unrelated handle is still alive. */
    for (int i = 0; i < 1024 && uv_loop_alive(ctx->loop); i++) {
      uv_run(ctx->loop, UV_RUN_NOWAIT);
    }

    if (uv_loop_close(ctx->loop) == 0) {
      free(ctx->loop);
      ctx->loop = NULL;
    } else {
      /* Loop still busy: avoid freeing an invalid loop object.
         Remaining resources are leaked intentionally for safety. */
#ifndef NDEBUG
      TLOG_DEBUG("uv_loop_close failed, resources leaked (EBUSY?)");
#endif
      ctx->loop = NULL;
    }
  }
  free(ctx);
}

const char *turbo_strerror(int err) { return uv_strerror(err); }

/* ── Post queue: thread-safe callback posting to event loop ── */

/**
 * @brief Callback invoked when uv_async_send() wakes the loop.
 * 
 * Drains all pending tasks from the lock-free ring buffer.
 * Good taste: No malloc, no mutex, just atomic reads.
 */
static void post_async_cb(uv_async_t *handle) {
  coro_context_t *ctx = (coro_context_t *)handle->data;
  
  int tail = t_atomic_load(&ctx->post_tail);
  int head = t_atomic_load(&ctx->post_head);
  
  /* Drain all pending slots */
  while (tail != head) {
    coro_post_slot_t *slot = &ctx->post_ring[tail];
    slot->fn(slot->arg);
    
    tail = (tail + 1) & (ctx->post_ring_size - 1);
    t_atomic_store(&ctx->post_tail, tail);
  }
}

/**
 * @brief Initialize the lock-free post queue.
 * 
 * Allocates a ring buffer (default 4096 slots) for cross-thread task posting.
 * Size must be power of 2 for efficient modulo via bitwise AND.
 */
static int ensure_post_queue(coro_context_t *ctx) {
  if (ctx->post_initialized) return 0;
  
  /* Default ring size: 16384 slots (can handle burst of 16383 concurrent posts) */
  ctx->post_ring_size = 16384;
  ctx->post_ring = (coro_post_slot_t *)calloc(ctx->post_ring_size, sizeof(coro_post_slot_t));
  if (!ctx->post_ring) return TURBO_ENOMEM;
  
  t_atomic_store(&ctx->post_head, 0);
  t_atomic_store(&ctx->post_tail, 0);
  
  int r = uv_async_init(ctx->loop, &ctx->post_async, post_async_cb);
  if (r != 0) {
    free(ctx->post_ring);
    ctx->post_ring = NULL;
    return r;
  }
  uv_unref((uv_handle_t *)&ctx->post_async); /* Don't keep loop alive just for the post queue */
  ctx->post_async.data = ctx;
  ctx->post_initialized = 1;
  return 0;
}

/**
 * @brief Post a callback to be executed on the context's event loop thread.
 * 
 * Thread-safe, lock-free implementation using ring buffer + atomic operations.
 * Good taste: No malloc, no mutex, just CAS on ring indices.
 * 
 * @param ctx Target context
 * @param fn Callback function
 * @param arg Opaque argument for callback
 * @return 0 on success, TURBO_EINVAL if invalid params, TURBO_ENOMEM if ring full
 */
int coro_post(coro_context_t *ctx, coro_post_fn fn, void *arg) {
  if (!ctx || !fn) return TURBO_EINVAL;

  if (!ctx->post_initialized) return TURBO_EINVAL;

  /* Lock-free enqueue: try once, fail fast if full */
  int head = t_atomic_load(&ctx->post_head);
  int tail = t_atomic_load(&ctx->post_tail);
  int next_head = (head + 1) & (ctx->post_ring_size - 1);
  
  /* Ring full? Return error for caller to handle */
  if (next_head == tail) {
    return TURBO_ENOMEM;
  }
  
  /* Write to slot (safe: only producer writes to head index) */
  ctx->post_ring[head].fn = fn;
  ctx->post_ring[head].arg = arg;
  
  /* Publish: advance head atomically */
  t_atomic_store(&ctx->post_head, next_head);

  return uv_async_send(&ctx->post_async);
}

void coro_context_set_persistent(coro_context_t *ctx, int persistent) {
  if (!ctx || !ctx->post_initialized) return;
  
  if (persistent && !ctx->persistent) {
    uv_ref((uv_handle_t *)&ctx->post_async);
    ctx->persistent = 1;
  } else if (!persistent && ctx->persistent) {
    uv_unref((uv_handle_t *)&ctx->post_async);
    ctx->persistent = 0;
  }
}

/* ── Managed coroutines: auto-cleanup on completion ────────── */

static void pooled_coro_cleanup(coro_t *co, void *arg) {
  coro_object_pool_t *pool = (coro_object_pool_t *)arg;
  coro_object_pool_release(pool, co);
}

int coro_context_spawn(coro_context_t *ctx, coro_fn fn, void *arg) {
  if (!ctx || !fn) return TURBO_EINVAL;

  coro_t *co = NULL;

  if (ctx->pool) {
    co = coro_object_pool_acquire(ctx->pool, fn, arg);
    if (co) {
      coro_set_cleanup(co, pooled_coro_cleanup, ctx->pool);
      coro_scheduler_adopt(ctx->scheduler, co);
    }
  }

  if (!co) {
    co = coro_spawn(ctx->scheduler, fn, arg, NULL);
  }

  return co ? 0 : TURBO_ENOMEM;
}

/* ── Lazy tasks: deferred execution ───────────────────────── */

/** Task state */
typedef enum {
  TASK_CREATED = 0, /**< Created but not started */
  TASK_STARTED,     /**< Started and running */
  TASK_COMPLETED,   /**< Completed successfully */
  TASK_CANCELLED    /**< Cancelled before start */
} task_state_t;

/** Internal task structure */
struct coro_task_s {
  coro_context_t *ctx; /**< Owning context */
  coro_fn fn;          /**< User entry function */
  void *arg;           /**< User argument */
  task_state_t state;  /**< Current state */
  int ref_count;       /**< Reference count (for when_all/when_any) */
};

/** Wrapper argument for task execution */
typedef struct {
  coro_task_t *task;
  coro_fn user_fn;
  void *user_arg;
} task_wrapper_arg_t;

/** Wrapper function that marks task as completed when done */
static void task_wrapper_fn(coro_t *co, void *arg) {
  task_wrapper_arg_t *wa = (task_wrapper_arg_t *)arg;
  /* Call user function */
  wa->user_fn(co, wa->user_arg);
  /* Mark done before freeing so cleanup_done_tasks can collect it */
  wa->task->state = TASK_COMPLETED;
  free(wa);
}

static void cleanup_done_tasks(coro_context_t *ctx) {
  if (!ctx || !ctx->tasks) return;

  /* Compact the array in-place, freeing tasks that are done and unreferenced.
     This keeps the array from growing unboundedly on high-task-churn workloads. */
  int write_idx = 0;
  for (int read_idx = 0; read_idx < ctx->task_count; read_idx++) {
    coro_task_t *task = ctx->tasks[read_idx];

    int is_done = (task->state == TASK_COMPLETED || task->state == TASK_CANCELLED);
    int can_free = (task->ref_count == 0);

    if (is_done && can_free) {
      /* task was malloc'd in coro_task_create — safe to free */
      free(task);
    } else {
      ctx->tasks[write_idx++] = task;
    }
  }
  ctx->task_count = write_idx;
}

static int add_task_to_context(coro_context_t *ctx, coro_task_t *task) {
  if (ctx->task_count >= ctx->task_capacity) {
    int new_capacity = ctx->task_capacity == 0 ? 8 : ctx->task_capacity * 2;
    coro_task_t **new_tasks = (coro_task_t **)realloc(ctx->tasks, new_capacity * sizeof(coro_task_t *));
    if (!new_tasks) return TURBO_ENOMEM;

    ctx->tasks = new_tasks;
    ctx->task_capacity = new_capacity;
  }
  ctx->tasks[ctx->task_count++] = task;
  return 0;
}

static void remove_task_from_context(coro_context_t *ctx, coro_task_t *task) {
  if (!ctx || !ctx->tasks || !task) return;

  for (int i = 0; i < ctx->task_count; i++) {
    if (ctx->tasks[i] == task) {
      for (int j = i + 1; j < ctx->task_count; j++) {
        ctx->tasks[j - 1] = ctx->tasks[j];
      }
      ctx->task_count--;
      return;
    }
  }
}

coro_task_t *coro_task_create(coro_context_t *ctx, coro_fn fn, void *arg) {
  if (!ctx || !fn) return NULL;

  /* Allocate task via malloc (not arena — tasks are individually freed) */
  coro_task_t *task = (coro_task_t *)calloc(1, sizeof(coro_task_t));
  if (!task) return NULL;

  task->ctx = ctx;
  task->fn = fn;
  task->arg = arg;
  task->state = TASK_CREATED;
  task->ref_count = 1;

  if (add_task_to_context(ctx, task) != 0) {
    return NULL;
  }

  return task;
}

int coro_task_start(coro_task_t *task) {
  if (!task) return TURBO_EINVAL;
  if (task->state != TASK_CREATED) return TURBO_EINVAL;

  task_wrapper_arg_t *wrapper_arg = malloc(sizeof(task_wrapper_arg_t));
  if (!wrapper_arg) return TURBO_ENOMEM;

  wrapper_arg->task = task;
  wrapper_arg->user_fn = task->fn;
  wrapper_arg->user_arg = task->arg;

  coro_t *co = coro_spawn(task->ctx->scheduler, task_wrapper_fn, wrapper_arg, NULL);
  if (!co) {
    free(wrapper_arg);
    return TURBO_ENOMEM;
  }

  task->state = TASK_STARTED;
  return 0;
}

int coro_task_cancel(coro_task_t *task) {
  if (!task) return TURBO_EINVAL;
  if (task->state != TASK_CREATED) return TURBO_EINVAL;

  task->state = TASK_CANCELLED;
  return 0;
}

int coro_task_is_done(coro_task_t *task) {
  if (!task) return 1;
  return (task->state == TASK_COMPLETED || task->state == TASK_CANCELLED);
}

void coro_task_destroy(coro_task_t *task) {
  if (!task) return;
  remove_task_from_context(task->ctx, task);
  /* The coroutine itself is managed by the scheduler; do not touch it here. */
  free(task);
}

/* ── Task combinators: composition ────────────────────────── */

int coro_when_all(coro_context_t *ctx, coro_task_t **tasks, int count) {
  /* Fast path: nothing to wait for */
  if (count <= 0) return TURBO_EINVAL;
  if (!ctx || !tasks) return TURBO_EINVAL;

  /* Must be called from inside a coroutine */
  ASSERT_IN_CORO();

  /* Hold references so cleanup_done_tasks won't free tasks under us */
  for (int i = 0; i < count; i++) {
    tasks[i]->ref_count++;
  }

  /* Yield until all tasks are done */
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
  /* Fast path: nothing to wait for */
  if (count <= 0) return TURBO_EINVAL;
  if (!ctx || !tasks) return TURBO_EINVAL;

  /* Must be called from inside a coroutine */
  ASSERT_IN_CORO();

  /* Hold references */
  for (int i = 0; i < count; i++) {
    tasks[i]->ref_count++;
  }

  /* Yield until the first task completes.
   * Winner is always the first completed task found in array order (FIFO).
   * If two tasks complete in the same scheduler tick, the one with the
   * smaller index wins deterministically. */
  int winner = -1;
  while (1) {
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
  uv_timer_t timer;
} sleep_ctx_t;

static void on_sleep_timer(uv_timer_t *handle) {
  sleep_ctx_t *sctx = (sleep_ctx_t *)handle->data;

  if (sctx->co_is_scheduled) {
    /* Scheduler-managed: clear waiting flag so scheduler resumes it */
    coro_set_waiting_for_io(sctx->co, 0);
  } else {
    /* Manually-managed: resume immediately */
    coro_resume(sctx->co);
  }

  uv_close((uv_handle_t *)handle, NULL);
  free(sctx);
}

void coro_sleep(coro_context_t *ctx, uint64_t ms) {
  if (!ctx) return;

  /* Must be called from inside a coroutine */
  ASSERT_IN_CORO();

  coro_t *co = coro_running();
  if (!co) return;

  /* Special case: 0ms = just yield to scheduler */
  if (ms == 0) {
    coro_yield();
    return;
  }

  /* Allocate sleep context */
  sleep_ctx_t *sctx = malloc(sizeof(sleep_ctx_t));
  if (!sctx) return;

  sctx->co = co;
  sctx->co_is_scheduled = coro_is_scheduled(co);

  /* Initialize timer */
  uv_timer_init(ctx->loop, &sctx->timer);
  sctx->timer.data = sctx;

  /* Mark coroutine as waiting for I/O if scheduler-managed */
  if (sctx->co_is_scheduled) {
    coro_set_waiting_for_io(co, 1);
  }

  /* Start timer and yield */
  uv_timer_start(&sctx->timer, on_sleep_timer, ms, 0);
  coro_yield();
}

/* ── Memory Pool ────────────────────────────────────────────── */

#include "turbo_buffer.h"
#include "turbo_atomic.h"

/**
 * @brief Global fallback memory pool for legacy code.
 * 
 * DEPRECATED: This global pool should NOT be used by new code.
 * All CoroNet components now require a coro_context_t with its own arena.
 * This function is kept only for backward compatibility with external code
 * that may still reference it, but will return NULL to force proper migration.
 * 
 * @return NULL (deprecated, use ctx->arena instead)
 */
void* coro_get_memory_pool(void) {
    /* Return NULL to force callers to use context-specific arenas.
       Good taste: fail fast rather than hide problems with fallbacks. */
    return NULL;
}
