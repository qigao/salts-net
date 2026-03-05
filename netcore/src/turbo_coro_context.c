/**
 * @file coro_context.c
 * @brief Implementation of the opaque event-loop context.
 *
 */

#include "turbo_coro_context.h"
#include "turbo_coro_internal.h"
#include "turbo_thread.h"
#include <stdlib.h>
#include <uv.h>

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

  /* Initialize lazy task list */
  ctx->tasks = NULL;
  ctx->task_count = 0;
  ctx->task_capacity = 0;

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

  /* Coroutine contexts are strictly single-threaded/cooperative per loop.
     Disable global synchronization overhead to improve performance. */
  turbo_sync_set_single_threaded(1);

  return ctx;
}

int coro_context_run(coro_context_t *ctx, turbo_run_mode_t mode) {
  if (!ctx || !ctx->loop) return 0;

  coro_context_t *prev = tls_current_context;
  tls_current_context = ctx;

  int r = 0;

  if (mode == TURBO_RUN_DEFAULT) {
    /* Run until both event loop and scheduler are idle */
    while (1) {
      /* Choose run mode:
       * - UV_RUN_NOWAIT: if we have ready coroutines, don't block; poll I/O and
       *                  immediately resume coroutines.
       * - UV_RUN_ONCE: if all active coroutines are blocked on I/O, we can
       *                safely block the thread until the next I/O event or
       *                timer fires.
       */
      int ready = ctx->scheduler ? coro_scheduler_has_ready(ctx->scheduler) : 0;
      r = uv_run(ctx->loop, ready ? UV_RUN_NOWAIT : UV_RUN_ONCE);

      /* Resume all managed coroutines */
      if (ctx->scheduler) {
        coro_scheduler_tick(ctx->scheduler);
      }

      /* Clean up completed tasks */
      cleanup_done_tasks(ctx);

      /* Exit criteria... */
      int has_work = uv_loop_alive(ctx->loop);
      int has_coros = ctx->scheduler ? coro_scheduler_count(ctx->scheduler) : 0;

      if (ctx->stop_requested || (!has_coros && (!has_work || r == 0))) {
        break;
      }
    }
  } else {
    /* Run one iteration as requested */
    r = uv_run(ctx->loop, (uv_run_mode)mode);

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
}

int coro_context_alive(coro_context_t *ctx) {
  if (!ctx) return 0;
  return uv_loop_alive(ctx->loop);
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

  /* Destroy all remaining lazy tasks */
  for (int i = 0; i < ctx->task_count; i++) {
    if (ctx->tasks[i]) {
      free(ctx->tasks[i]);
      ctx->tasks[i] = NULL;
    }
  }
  free(ctx->tasks);
  ctx->tasks = NULL;
  ctx->task_count = 0;
  ctx->task_capacity = 0;

  if (ctx->post_initialized) {
    uv_close((uv_handle_t *)&ctx->post_async, NULL);
    /* Drain any remaining post queue nodes */
    coro_post_node_t *node = ctx->post_head;
    while (node) {
      coro_post_node_t *next = node->next;
      free(node);
      node = next;
    }
    ctx->post_head = NULL;
    ctx->post_tail = NULL;
    turbo_mutex_destroy(&ctx->post_mutex);
    ctx->post_initialized = 0;
  }
  if (ctx->owns_loop && ctx->loop) {
    /* Drain pending close callbacks (e.g. post_async/timer closes)
       before closing the owned loop. */
    for (int i = 0; i < 1024 && uv_loop_alive(ctx->loop); i++) {
      uv_run(ctx->loop, UV_RUN_NOWAIT);
    }

    if (uv_loop_close(ctx->loop) == 0) {
      free(ctx->loop);
      ctx->loop = NULL;
    } else {
      /* Loop still busy: avoid freeing an invalid loop object.
         Remaining resources are leaked intentionally for safety. */
      ctx->loop = NULL;
    }
  }
  free(ctx);
}

const char *turbo_strerror(int err) { return uv_strerror(err); }

/* ── Post queue: thread-safe callback posting to event loop ── */

static void post_async_cb(uv_async_t *handle) {
  coro_context_t *ctx = (coro_context_t *)handle->data;
  coro_post_node_t *head;

  turbo_mutex_lock(&ctx->post_mutex);
  head = ctx->post_head;
  ctx->post_head = NULL;
  ctx->post_tail = NULL;
  turbo_mutex_unlock(&ctx->post_mutex);

  while (head) {
    coro_post_node_t *node = head;
    head = head->next;
    node->fn(node->arg);
    free(node);
  }
}

static int ensure_post_queue(coro_context_t *ctx) {
  if (ctx->post_initialized) return 0;
  turbo_mutex_init(&ctx->post_mutex);
  int r = uv_async_init(ctx->loop, &ctx->post_async, post_async_cb);
  if (r != 0) {
    turbo_mutex_destroy(&ctx->post_mutex);
    return r;
  }
  ctx->post_async.data = ctx;
  ctx->post_initialized = 1;
  return 0;
}

int coro_post(coro_context_t *ctx, coro_post_fn fn, void *arg) {
  if (!ctx || !fn) return TURBO_EINVAL;

  int r = ensure_post_queue(ctx);
  if (r != 0) return r;

  coro_post_node_t *node = malloc(sizeof(coro_post_node_t));
  if (!node) return TURBO_ENOMEM;
  node->fn = fn;
  node->arg = arg;
  node->next = NULL;

  turbo_mutex_lock(&ctx->post_mutex);
  if (ctx->post_tail) {
    ctx->post_tail->next = node;
  } else {
    ctx->post_head = node;
  }
  ctx->post_tail = node;
  turbo_mutex_unlock(&ctx->post_mutex);

  return uv_async_send(&ctx->post_async);
}

/* ── Managed coroutines: auto-cleanup on completion ────────── */

int coro_context_spawn(coro_context_t *ctx, coro_fn fn, void *arg) {
  if (!ctx || !fn) return TURBO_EINVAL;

  /* Adds to scheduler tail; runs on the next scheduler tick (lazy by design).
     Documents as "eager" relative to coro_task_create, which requires
     an explicit coro_task_start() call. */
  coro_t *co = coro_spawn(ctx->scheduler, fn, arg, NULL);
  if (!co) return TURBO_ENOMEM;

  return 0;
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
    coro_task_t **new_tasks = realloc(ctx->tasks, new_capacity * sizeof(coro_task_t *));
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

  coro_task_t *task = calloc(1, sizeof(coro_task_t));
  if (!task) return NULL;

  task->ctx = ctx;
  task->fn = fn;
  task->arg = arg;
  task->state = TASK_CREATED;
  task->ref_count = 1; /* Caller holds one reference */

  int r = add_task_to_context(ctx, task);
  if (r != 0) {
    free(task);
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
