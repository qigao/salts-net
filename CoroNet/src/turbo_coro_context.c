/**
 * @file coro_context.c
 * @brief Implementation of the opaque event-loop context.
 *
 * Uses turbo_loop_t (native IOCP/epoll/kqueue) instead of libuv.
 */

#include "platform.h"
#include "turbo_coro_context.h"
#include "CoroNet/turbo_coro_object_pool.h"
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

enum {
  CORO_POST_QUEUE_CAPACITY = 16384,
  CORO_POST_QUEUE_USABLE_CAPACITY = CORO_POST_QUEUE_CAPACITY - 1
};

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
  coro_fn fn;
  void *arg;
  size_t context_index;
  int ref_count;
  uint8_t started;
  uint8_t cancelled;
  uint8_t done;
};

void *coro_context_native_loop(const coro_context_t *ctx) { return ctx ? (void *)ctx->loop : NULL; }

/* ── Forward declarations ───────────────────────────────────── */
static void cleanup_done_tasks(coro_context_t *ctx);

static void drain_platform_completions(coro_context_t *ctx) {
#ifdef _WIN32
  if (ctx && ctx->iocp_pool) {
    (void)iocp_pool_drain(ctx->iocp_pool);
  }
#else
  (void)ctx;
#endif
}
static int remove_task_from_context(coro_context_t *ctx, coro_task_t *task);
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

  turbo_vec_destroy(&ctx->tasks);

  if (ctx->post_queue) {
    if (ctx->post_initialized) {
      disruptor_consumer_unregister(ctx->post_queue, &ctx->post_consumer);
    }
    disruptor_destroy(ctx->post_queue);
    ctx->post_queue = NULL;
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

coro_context_t *coro_context_create_ex(
    void *loop, const struct coro_object_pool_config_s *pool_config) {
  coro_context_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;
  atomic_init(&ctx->stop_requested, 0);

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

  /* Initialize lazy task list. Reserve the historical initial capacity. */
  if (turbo_vec_init(&ctx->tasks, sizeof(coro_task_t *)) != TURBO_OK ||
      turbo_vec_reserve(&ctx->tasks, 8) != TURBO_OK) {
    coro_context_cleanup_create_failure(ctx);
    return NULL;
  }

  /* Initialize scheduler for managed coroutines */
  ctx->scheduler = coro_scheduler_create();
  if (!ctx->scheduler) {
    coro_context_cleanup_create_failure(ctx);
    return NULL;
  }

  /* Create coroutine pool with context's arena */
  ctx->pool = coro_object_pool_create(pool_config, ctx);
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
  ctx->stream_recv_buffer_size = CORO_CONTEXT_DEFAULT_STREAM_RECV_BUFFER_SIZE;

  return ctx;
}

coro_context_t *coro_context_create(void *loop) { return coro_context_create_ex(loop, NULL); }

int coro_context_run(coro_context_t *ctx, turbo_run_mode_t mode) {
  if (!ctx || !ctx->loop) return 0;

  coro_context_t *prev = tls_current_context;
  tls_current_context = ctx;

  /* stop_requested is a one-shot signal for the current run() call.
   * If we keep it sticky, the next sync API invocation can return
   * immediately without actually draining its work. */
  atomic_store_explicit(&ctx->stop_requested, 0, memory_order_release);

  int r = 0;

  if (mode == TURBO_RUN_DEFAULT) {
    while (1) {
      int has_ready = coro_scheduler_has_ready(ctx->scheduler);

      int stop_requested = atomic_load_explicit(&ctx->stop_requested, memory_order_acquire);

      if (!stop_requested && !context_loop_alive(ctx) && !coro_scheduler_count(ctx->scheduler) &&
          atomic_load_explicit(&ctx->external_refs, memory_order_acquire) == 0 &&
          post_queue_empty(ctx)) {
        break;
      }

      /* A post may arrive before run() enters poll, so the wake alone cannot
       * guarantee progress. Observe the queue before choosing an infinite wait. */
      if (stop_requested || has_ready || !post_queue_empty(ctx)) {
        turbo_loop_poll(ctx->loop, 2, 0); /* NOWAIT */
      } else {
        turbo_loop_poll(ctx->loop, -1, 1); /* block until wake */
      }

      drain_platform_completions(ctx);
      drain_post_queue(ctx);

      if (ctx->scheduler) {
        coro_scheduler_tick(ctx->scheduler);
      }
      cleanup_done_tasks(ctx);

      stop_requested = atomic_load_explicit(&ctx->stop_requested, memory_order_acquire);
      if ((stop_requested && post_queue_empty(ctx)) ||
          (!context_loop_alive(ctx) && !coro_scheduler_count(ctx->scheduler) &&
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

    drain_platform_completions(ctx);
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
  atomic_store_explicit(&ctx->stop_requested, 1, memory_order_release);
  turbo_loop_stop(ctx->loop);
  turbo_loop_wake(ctx->loop);
}

int coro_context_alive(coro_context_t *ctx) {
  if (!ctx) return 0;
  if (atomic_load_explicit(&ctx->external_refs, memory_order_acquire) != 0) return 1;
  if (context_loop_alive(ctx)) return 1;
  if (ctx->scheduler && coro_scheduler_count(ctx->scheduler) > 0) return 1;
  if (!post_queue_empty(ctx)) return 1;
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
  if (!ctx || !ctx->post_initialized) return 1;
  return atomic_load_explicit((atomic_int *)&ctx->post_count, memory_order_acquire) == 0;
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

    waiting_for_shutdown = atomic_load_explicit(&ctx->external_refs, memory_order_acquire) != 0 ||
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
        (!ctx->scheduler || coro_scheduler_count(ctx->scheduler) == 0) && post_queue_empty(ctx)) {
      forced_teardown = 0;
      break;
    }
  }

  tls_current_context = prev;

  if (forced_teardown &&
      (atomic_load_explicit(&ctx->external_refs, memory_order_acquire) != 0 ||
       context_loop_alive(ctx) || (ctx->scheduler && coro_scheduler_count(ctx->scheduler) > 0) ||
       !post_queue_empty(ctx))) {
    TLOG_WARN("coro_context_destroy: forced teardown with pending loop work "
              "(ctx={}, external_refs={}, loop_alive={}, coro_count={}, post_empty={})",
              (void *)ctx, atomic_load_explicit(&ctx->external_refs, memory_order_acquire),
              turbo_loop_alive(ctx->loop),
              ctx->scheduler ? coro_scheduler_count(ctx->scheduler) : 0, post_queue_empty(ctx));
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
  coro_task_t **tasks = (coro_task_t **)turbo_vec_data(&ctx->tasks);
  for (size_t i = 0; i < turbo_vec_size(&ctx->tasks); ++i) {
    destroy_task_now(tasks[i]);
  }
  turbo_vec_destroy(&ctx->tasks);

  /* Cleanup arena: destroy and free since each context owns its arena */
  if (ctx->arena && ctx->owns_arena) {
    mem_destroy(ctx->arena);
    free(ctx->arena);
    ctx->arena = NULL;
  }

  /* Destroy the post queue after all external producers have stopped. */
  if (ctx->post_queue) {
    if (ctx->post_initialized) {
      disruptor_consumer_unregister(ctx->post_queue, &ctx->post_consumer);
    }
    disruptor_destroy(ctx->post_queue);
    ctx->post_queue = NULL;
    ctx->post_initialized = 0;
  }

  if (ctx->owns_loop && ctx->loop) {
    turbo_loop_destroy(ctx->loop);
    ctx->loop = NULL;
  }
  free(ctx);
}

/* ── Post queue: thread-safe callback posting to event loop ── */

/** @brief Drain all published callbacks on the event-loop thread. */
static void drain_post_queue(coro_context_t *ctx) {
  if (!ctx->post_initialized) return;

  for (;;) {
    disruptor_cursor_t available = {ctx->post_consumer_sequence};

    if (!disruptor_consumer_wait_for_nonblocking(ctx->post_queue, &available)) {
      /* Close the wake-covered interval, then recheck publication. A producer
       * racing this store either becomes visible here or performs the next wake. */
      atomic_store_explicit(&ctx->post_wake_pending, 0, memory_order_release);
      available.sequence = ctx->post_consumer_sequence;
      if (!disruptor_consumer_wait_for_nonblocking(ctx->post_queue, &available)) return;
      (void)atomic_exchange_explicit(&ctx->post_wake_pending, 1, memory_order_acq_rel);
    }

    while (ctx->post_consumer_sequence <= available.sequence) {
      disruptor_cursor_t current = {ctx->post_consumer_sequence};
      const coro_post_slot_t *entry =
          (const coro_post_slot_t *)disruptor_show_entry(ctx->post_queue, &current);
      coro_post_slot_t slot = *entry;

      /* Release capacity before invoking user code so a callback may reenter
       * coro_post() with the same queue-full behavior as the previous ring. */
      disruptor_consumer_release_entry(ctx->post_queue, &ctx->post_consumer, &current);
      ctx->post_consumer_sequence++;
      atomic_fetch_sub_explicit(&ctx->post_count, 1, memory_order_release);
      slot.fn(slot.arg1, slot.arg2);
    }
  }
}

/** @brief Initialize the MPSC post queue. */
static int ensure_post_queue(coro_context_t *ctx) {
  disruptor_config_t config;

  if (ctx->post_initialized) return 0;

  config.entry_size = sizeof(coro_post_slot_t);
  config.capacity = CORO_POST_QUEUE_CAPACITY;
  config.consumer_capacity = 1;
  config.mode = DISRUPTOR_MODE_BROADCAST;
  ctx->post_queue = disruptor_create(&config);
  if (!ctx->post_queue) return TURBO_ENOMEM;
  if (!disruptor_consumer_try_register(ctx->post_queue, &ctx->post_consumer,
                                       &ctx->post_consumer_sequence)) {
    disruptor_destroy(ctx->post_queue);
    ctx->post_queue = NULL;
    return TURBO_ENOMEM;
  }

  atomic_store_explicit(&ctx->post_count, 0, memory_order_relaxed);
  atomic_store_explicit(&ctx->post_wake_pending, 0, memory_order_relaxed);

  ctx->post_initialized = 1;
  return 0;
}

/**
 * @brief Post a callback to be executed on the context's event loop thread.
 *
 * Thread-safe MPSC Disruptor with one event-loop consumer.
 */
int coro_post(coro_context_t *ctx, coro_post_fn fn, void *arg1, void *arg2) {
  disruptor_cursor_t cursor = {0};
  coro_post_slot_t *entry;
  int count;

  if (!ctx || !fn) return TURBO_EINVAL;

  if (!ctx->post_initialized) return TURBO_EINVAL;

  count = atomic_load_explicit(&ctx->post_count, memory_order_acquire);
  do {
    if (count >= CORO_POST_QUEUE_USABLE_CAPACITY) return TURBO_ENOMEM;
  } while (!atomic_compare_exchange_weak_explicit(&ctx->post_count, &count, count + 1,
                                                  memory_order_acq_rel, memory_order_acquire));

  /* The logical reservation above keeps at least one physical slot unused, so
   * this blocking claim cannot wait for consumer capacity. */
  disruptor_publisher_next_entry_blocking(ctx->post_queue, &cursor);
  entry = (coro_post_slot_t *)disruptor_acquire_entry(ctx->post_queue, &cursor);
  entry->fn = fn;
  entry->arg1 = arg1;
  entry->arg2 = arg2;
  (void)disruptor_publisher_publish(ctx->post_queue, &cursor);

  if (atomic_exchange_explicit(&ctx->post_wake_pending, 1, memory_order_acq_rel) == 0) {
    turbo_loop_wake(ctx->loop);
  }
  return TURBO_OK;
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

int coro_context_set_stream_recv_buffer_size(coro_context_t *ctx, size_t bytes) {
  if (!ctx || bytes == 0u) return TURBO_EINVAL;
  ctx->stream_recv_buffer_size = bytes;
  return TURBO_OK;
}

size_t coro_context_get_stream_recv_buffer_size(const coro_context_t *ctx) {
  return ctx ? ctx->stream_recv_buffer_size : CORO_CONTEXT_DEFAULT_STREAM_RECV_BUFFER_SIZE;
}

int coro_context_get_last_error(const coro_context_t *ctx) { return ctx ? ctx->last_error : 0; }

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

/* Each task stores its dense-vector index. Swap removal is O(1) time and
 * updates the moved task so the vector remains the single ownership source. */
static int remove_task_from_context(coro_context_t *ctx, coro_task_t *task) {
  coro_task_t **tasks;
  coro_task_t *moved;
  size_t count;
  size_t index;

  if (!ctx || !task) return TURBO_EINVAL;

  count = turbo_vec_size(&ctx->tasks);
  index = task->context_index;
  tasks = (coro_task_t **)turbo_vec_data(&ctx->tasks);
  if (index >= count || !tasks || tasks[index] != task) return TURBO_EINVAL;

  moved = tasks[count - 1];
  if (turbo_vec_swap_remove(&ctx->tasks, index, NULL) != TURBO_OK) return TURBO_EINVAL;
  if (moved != task) moved->context_index = index;
  task->context_index = SIZE_MAX;
  return TURBO_OK;
}

static int register_task_with_context(coro_context_t *ctx, coro_task_t *task) {
  int rc;

  if (!ctx || !task) return TURBO_EINVAL;

  rc = turbo_vec_push(&ctx->tasks, &task);
  if (rc != TURBO_OK) return rc;
  task->context_index = turbo_vec_size(&ctx->tasks) - 1;
  return TURBO_OK;
}

static void cleanup_done_tasks(coro_context_t *ctx) {
  size_t i = turbo_vec_size(&ctx->tasks);
  while (i > 0) {
    coro_task_t **tasks = (coro_task_t **)turbo_vec_data(&ctx->tasks);
    coro_task_t *task = tasks[--i];
    if (coro_task_is_done(task) && task->ref_count == 0) {
      if (remove_task_from_context(ctx, task) == TURBO_OK) {
        destroy_task_now(task);
      }
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
  task->ctx = ctx;
  task->fn = fn;
  task->arg = arg;
  task->context_index = SIZE_MAX;
  task->started = 0;
  task->cancelled = 0;
  task->done = 0;
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

int coro_task_is_done(coro_task_t *task) { return task ? task->done : 1; }

static void destroy_task_now(coro_task_t *task) { free(task); }

void coro_task_destroy(coro_task_t *task) {
  if (!task) return;

  if (task->ref_count > 0) {
    task->ref_count--;
  }

  if (task->ref_count == 0 && (task->done || !task->started)) {
    if (task->ctx && task->context_index != SIZE_MAX) {
      if (remove_task_from_context(task->ctx, task) != TURBO_OK) return;
    }
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

enum { CORO_WAIT_IDLE = 0, CORO_WAIT_ACTIVE, CORO_WAIT_COMPLETING };

struct coro_wait_s {
  coro_t *co;
  int co_is_scheduled;
  coro_context_t *ctx;
  turbo_timer_t *timer;
  turbo_mutex_t mutex;
  atomic_int state;
  atomic_int status;
};

static void coro_wait_complete_on_context(void *arg1, void *arg2) {
  (void)arg2;
  coro_wait_t *wait = (coro_wait_t *)arg1;
  coro_t *co = wait->co;
  int co_is_scheduled = wait->co_is_scheduled;

  turbo_timer_destroy(wait->timer);
  wait->timer = NULL;
  wait->co = NULL;
  wait->co_is_scheduled = 0;
  coro_context_release_external(wait->ctx);

  if (co_is_scheduled) {
    coro_set_waiting_for_io(co, 0);
  } else {
    coro_resume(co);
  }
}

static int coro_wait_claim_completion(coro_wait_t *wait, int status) {
  int expected = CORO_WAIT_ACTIVE;

  if (!atomic_compare_exchange_strong_explicit(&wait->state, &expected, CORO_WAIT_COMPLETING,
                                               memory_order_acq_rel, memory_order_acquire)) {
    return TURBO_EALREADY;
  }
  atomic_store_explicit(&wait->status, status, memory_order_release);
  return TURBO_OK;
}

static void coro_wait_post_completion(coro_wait_t *wait) {
  int rc;

  rc = coro_post(wait->ctx, coro_wait_complete_on_context, wait, NULL);
  while (rc == TURBO_ENOMEM) {
    turbo_thread_yield();
    rc = coro_post(wait->ctx, coro_wait_complete_on_context, wait, NULL);
  }
}

static void on_coro_wait_timer(turbo_timer_t *timer) {
  coro_wait_t *wait = (coro_wait_t *)turbo_timer_get_data(timer);
  if (wait && coro_wait_claim_completion(wait, TURBO_OK) == TURBO_OK) {
    coro_wait_post_completion(wait);
  }
}

coro_wait_t *coro_wait_create(coro_context_t *ctx) {
  coro_wait_t *wait;
  if (!ctx) return NULL;
  wait = (coro_wait_t *)calloc(1, sizeof(*wait));
  if (!wait) return NULL;
  wait->ctx = ctx;
  turbo_mutex_init(&wait->mutex);
  atomic_init(&wait->state, CORO_WAIT_IDLE);
  atomic_init(&wait->status, TURBO_OK);
  return wait;
}

int coro_wait_destroy(coro_wait_t *wait) {
  if (!wait) return TURBO_EINVAL;
  if (atomic_load_explicit(&wait->state, memory_order_acquire) != CORO_WAIT_IDLE) {
    return TURBO_EBUSY;
  }
  turbo_mutex_destroy(&wait->mutex);
  free(wait);
  return TURBO_OK;
}

int coro_wait_for(coro_wait_t *wait, uint64_t ms) {
  int rc;
  if (!wait || !wait->ctx) return TURBO_EINVAL;

  ASSERT_IN_CORO();

  if (!coro_running()) return TURBO_EINVAL;

  if (ms == 0) {
    coro_yield();
    return TURBO_OK;
  }

  turbo_mutex_lock(&wait->mutex);
  if (atomic_load_explicit(&wait->state, memory_order_acquire) != CORO_WAIT_IDLE) {
    turbo_mutex_unlock(&wait->mutex);
    return TURBO_EBUSY;
  }
  wait->co = coro_running();
  atomic_store_explicit(&wait->status, TURBO_OK, memory_order_release);
  wait->co_is_scheduled = coro_is_scheduled(wait->co);
  wait->timer = turbo_timer_create(NULL);
  if (!wait->timer) {
    wait->co = NULL;
    wait->co_is_scheduled = 0;
    turbo_mutex_unlock(&wait->mutex);
    return TURBO_ENOMEM;
  }

  turbo_timer_set_data(wait->timer, wait);

  if (wait->co_is_scheduled) {
    coro_set_waiting_for_io(wait->co, 1);
  }

  coro_context_acquire_external(wait->ctx);
  atomic_store_explicit(&wait->state, CORO_WAIT_ACTIVE, memory_order_release);
  rc = turbo_timer_start(wait->timer, on_coro_wait_timer, ms, 0);
  if (rc != TURBO_OK) {
    if (wait->co_is_scheduled) {
      coro_set_waiting_for_io(wait->co, 0);
    }
    coro_context_release_external(wait->ctx);
    turbo_timer_destroy(wait->timer);
    wait->timer = NULL;
    wait->co = NULL;
    wait->co_is_scheduled = 0;
    atomic_store_explicit(&wait->state, CORO_WAIT_IDLE, memory_order_release);
    turbo_mutex_unlock(&wait->mutex);
    return TURBO_EIO;
  }
  turbo_mutex_unlock(&wait->mutex);
  coro_yield();
  rc = atomic_load_explicit(&wait->status, memory_order_acquire);
  atomic_store_explicit(&wait->state, CORO_WAIT_IDLE, memory_order_release);
  return rc;
}

int coro_wait_interrupt(coro_wait_t *wait, int status) {
  int rc;
  if (!wait || status == TURBO_OK) return TURBO_EINVAL;
  turbo_mutex_lock(&wait->mutex);
  rc = coro_wait_claim_completion(wait, status);
  if (rc == TURBO_OK) (void)turbo_timer_stop(wait->timer);
  turbo_mutex_unlock(&wait->mutex);
  if (rc != TURBO_OK) return rc;
  coro_wait_post_completion(wait);
  return TURBO_OK;
}

void coro_sleep(coro_context_t *ctx, uint64_t ms) {
  coro_wait_t *wait;
  if (!ctx) return;
  wait = coro_wait_create(ctx);
  if (!wait) return;
  (void)coro_wait_for(wait, ms);
  (void)coro_wait_destroy(wait);
}

#ifndef _WIN32
  #include <fcntl.h>
  #include <poll.h>
  #include <unistd.h>
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
    while (read(loop->wake_fds[0], &val, sizeof(val)) > 0)
      ;
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

void turbo_loop_ref(turbo_loop_t *loop) {
  if (loop) loop->ref_count++;
}
void turbo_loop_unref(turbo_loop_t *loop) {
  if (loop && loop->ref_count > 0) loop->ref_count--;
}

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
coro_context_t *coro_context_current(void) { return tls_current_context; }
void *coro_get_memory_pool(void) { return tls_current_context ? tls_current_context->arena : NULL; }
void *coro_context_get_arena(coro_context_t *ctx) { return ctx ? ctx->arena : NULL; }

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
