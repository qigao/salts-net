/**
 * @file coro_context.h
 * @brief Opaque event-loop context for coroutine-based networking.
 *
 * Users create a context, pass it to client/server constructors,
 * and run/stop it. No libuv types leak into the public API.
 */

#ifndef coro_CONTEXT_H
#define coro_CONTEXT_H

#include "platform.h"
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/** Opaque event-loop context (wraps libuv loop + thread-safe post queue) */
typedef struct coro_context_s coro_context_t;

/** Shorter alias for coro_context_t */
typedef struct coro_context_s coro_context;

/**
 * @brief Create an event-loop context.
 *
 * @param loop  Existing uv_loop_t* to wrap (cast to void* for ABI safety),
 *              or NULL to allocate and own a fresh loop.
 *
 * When @p loop is NULL the context allocates its own loop and closes it on
 * coro_context_destroy().  When @p loop is non-NULL the caller retains
 * ownership — the context will NOT close or free it on destroy.
 *
 * @return Context handle or NULL on failure
 */
CXX_C_API coro_context_t *coro_context_create(void *loop);

/**
 * @brief Destroy the context and free resources.
 *
 * If the context owns the loop (created with loop=NULL), the loop is
 * closed and freed. If wrapping an existing loop, only the
 * context struct is freed.
 *
 * @param ctx  Context to destroy (NULL-safe)
 */
CXX_C_API void coro_context_destroy(coro_context_t *ctx);

/**
 * @brief Controls how coro_context_run() drives the event loop.
 *
 * Values mirror libuv's uv_run_mode so the implementation can forward
 * them directly; a _Static_assert in coro_context.c guards against drift.
 */
typedef enum turbo_run_mode_e {
  /** Block until all handles are done or _stop() is called. */
  TURBO_RUN_DEFAULT = 0,
  /** Process one iteration (polls for I/O with a brief wait), then return. */
  TURBO_RUN_ONCE = 1,
  /** Process already-pending callbacks only; never block for I/O. */
  TURBO_RUN_NOWAIT = 2
} turbo_run_mode_t;

/**
 * @brief Drive the event loop.
 *
 * | mode                | behaviour                                          |
 * |---------------------|----------------------------------------------------|
 * | TURBO_RUN_DEFAULT   | Blocks until no active handles or _stop() is called |
 * | TURBO_RUN_ONCE      | One I/O poll iteration, then returns               |
 * | TURBO_RUN_NOWAIT    | Flushes pending callbacks without blocking         |
 *
 * @param ctx   Context to run
 * @param mode  Execution mode (see turbo_run_mode_t)
 * @return 0 when the loop is idle, non-zero if active handles remain
 */
CXX_C_API int coro_context_run(coro_context_t *ctx, turbo_run_mode_t mode);

/**
 * @brief Stop the event loop.
 *
 * Causes a running coro_context_run() to return on the
 * next iteration. Can be called from any thread.
 *
 * @param ctx  Context to stop
 */
CXX_C_API void coro_context_stop(coro_context_t *ctx);

/**
 * @brief Set whether the context should remain alive when idle.
 *
 * In persistent mode, the internal post-queue handle is 'ref'd', meaning
 * coro_context_run(DEFAULT) will block indefinitely waiting for tasks
 * even if there are no active coroutines or I/O.
 *
 * This is primarily used for worker threads in a thread pool.
 *
 * @param ctx         Context to modify
 * @param persistent  1 for persistent (keeps loop alive), 0 for transient
 */
CXX_C_API void coro_context_set_persistent(coro_context_t *ctx, int persistent);

// =============================================================================
// Query
// =============================================================================

/**
 * @brief Check if the event loop has active handles or requests.
 * @param ctx  Context to query
 * @return 1 if alive (has work to do), 0 if idle
 */
CXX_C_API int coro_context_alive(coro_context_t *ctx);

/**
 * @brief Get the number of active coroutines in this context.
 * @param ctx  Context to query
 * @return Count of alive coroutines
 */
CXX_C_API int coro_context_coro_count(coro_context_t *ctx);

/**
 * @brief Get the cached event-loop timestamp (milliseconds).
 *
 * Updated once per loop iteration — zero syscall overhead.
 * Useful for timeouts, rate limiting, and relative timing.
 *
 * @param ctx  Context to query
 * @return Monotonic time in milliseconds
 */
CXX_C_API uint64_t coro_context_now(coro_context_t *ctx);

/** Callback type for coro_post(). */
typedef void (*coro_post_fn)(void *arg1, void *arg2);

/**
 * @brief Post a callback to the event loop thread (thread-safe).
 *
 * Can be called from any thread. The callback runs on the next
 * event loop iteration in the thread that owns @p ctx.
 *
 * @param ctx  Event-loop context
 * @param fn   Callback to invoke on the loop thread
 * @param arg1 First opaque argument
 * @param arg2 Second opaque argument
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int coro_post(coro_context_t *ctx, coro_post_fn fn, void *arg1, void *arg2);

/**
 * @brief Return a human-readable error string for an error code.
 * @param err  Error code (TURBO_* or libuv-compatible)
 * @return Static string describing the error
 */
CXX_C_API const char *turbo_strerror(int err);

/**
 * @brief Get the current thread's event-loop context.
 *
 * This allows components to implicitly find the event loop they are running on,
 * avoiding the need to pass the context explicitly everywhere.
 *
 * @return Current context or NULL if no context is active on this thread.
 */
CXX_C_API coro_context_t *coro_context_current(void);

// =============================================================================
// Managed Coroutines (Auto-cleanup)
// =============================================================================

/** Forward declaration for coroutine type */
typedef struct coro_s coro_t;

/** Coroutine entry function */
typedef void (*coro_fn)(coro_t *co, void *arg);

/**
 * @brief Spawn a managed coroutine that auto-cleans on completion.
 *
 * The coroutine is tracked by the context's built-in scheduler and
 * automatically destroyed when it finishes.  Do NOT call
 * coro_destroy() on coroutines spawned this way.
 *
 * @note The coroutine runs on the **next** scheduler tick, not immediately.
 *       Use coro_task_create() + coro_task_start() for explicit
 *       control over when execution begins.
 *
 * @param ctx  Event-loop context
 * @param fn   Coroutine entry function
 * @param arg  Argument passed to fn
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int coro_context_spawn(coro_context_t *ctx, coro_fn fn, void *arg);

// =============================================================================
// Lazy Tasks (Deferred Execution)
// =============================================================================

/** Opaque lazy task handle */
typedef struct coro_task_s coro_task_t;

/**
 * @brief Create a lazy task (does not start execution).
 *
 * The task is created but not started. Call coro_task_start() to
 * begin execution. This allows conditional execution, cancellation, and
 * composition of multiple tasks.
 *
 * The task is automatically destroyed when it completes or is cancelled.
 *
 * @param ctx  Event-loop context
 * @param fn   Coroutine entry function
 * @param arg  Argument passed to fn
 * @return Task handle or NULL on failure
 */
CXX_C_API coro_task_t *coro_task_create(coro_context_t *ctx,
                                                     coro_fn fn,
                                                     void *arg);

/**
 * @brief Start a lazy task.
 *
 * Begins execution of a previously created task. The task must not have
 * been started or cancelled.
 *
 * @param task  Task to start
 * @return 0 on success, TURBO_EINVAL if already started/cancelled
 */
CXX_C_API int coro_task_start(coro_task_t *task);

/**
 * @brief Cancel a lazy task.
 *
 * Cancels a task that has not yet been started. Once cancelled, the task
 * cannot be started and will be automatically destroyed.
 *
 * @param task  Task to cancel
 * @return 0 on success, TURBO_EINVAL if already started
 */
CXX_C_API int coro_task_cancel(coro_task_t *task);

/**
 * @brief Check if a task has completed.
 *
 * @param task  Task to check
 * @return 1 if completed, 0 if still running or not started
 */
CXX_C_API int coro_task_is_done(coro_task_t *task);

/**
 * @brief Manually destroy a task.
 *
 * Normally tasks are auto-destroyed when they complete or are cancelled.
 * Use this only if you need to destroy a task that was created but never
 * started and never cancelled.
 *
 * @param task  Task to destroy (NULL-safe)
 */
CXX_C_API void coro_task_destroy(coro_task_t *task);

// =============================================================================
// Task Combinators (Composition)
// =============================================================================

/**
 * @brief Wait for all tasks to complete.
 *
 * Suspends the current coroutine until all tasks in the array have finished.
 * Tasks must be already started before calling this function.
 *
 * @note **Must be called from within a coroutine.** Calling from main() or a
 *       plain callback will corrupt minicoro state (asserted in debug builds).
 *
 * @note If @p count is 0, returns TURBO_EINVAL immediately.
 *
 * @code
 *   coro_task_t *t1 = coro_task_create(ctx, fetch_user, &user);
 *   coro_task_t *t2 = coro_task_create(ctx, fetch_posts, &posts);
 *
 *   coro_task_start(t1);
 *   coro_task_start(t2);
 *
 *   coro_when_all(ctx, (coro_task_t *[]){t1, t2}, 2);
 *   // Now both sets of data are ready
 * @endcode
 *
 * @param ctx    Event-loop context
 * @param tasks  Array of task pointers
 * @param count  Number of tasks (0 = no-op, returns 0)
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int coro_when_all(coro_context_t *ctx,
                                   coro_task_t **tasks,
                                   int count);

/**
 * @brief Wait for any task to complete (first-one-wins race).
 *
 * Suspends the current coroutine until at least one task finishes.
 * Returns the index of the first task that completed.
 *
 * Other tasks continue running in the background. Cancel them manually if
 * desired after this function returns.
 *
 * @note **Must be called from within a coroutine** (asserted in debug builds).
 *
 * @note **Tie-breaking**: if two tasks complete within the same scheduler
 *       tick, the one with the **smallest array index** wins.  This is
 *       deterministic and documented as FIFO/index-order.
 *
 * @note If @p count is 0, returns TURBO_EINVAL immediately.
 *
 * @code
 *   coro_task_t *fetch   = coro_task_create(ctx, fetch_data, &result);
 *   coro_task_t *timeout = coro_task_create(ctx, sleep_5s, NULL);
 *
 *   coro_task_start(fetch);
 *   coro_task_start(timeout);
 *
 *   int winner = coro_when_any(ctx, (coro_task_t *[]){fetch, timeout}, 2);
 *   if (winner == 0) { printf("Got data: %s\n", result); }
 *   else             { printf("Timed out\n"); }
 * @endcode
 *
 * @param ctx    Event-loop context
 * @param tasks  Array of task pointers
 * @param count  Number of tasks (must be > 0)
 * @return Index of first completed task (0..count-1), or negative error code
 */
CXX_C_API int coro_when_any(coro_context_t *ctx,
                                   coro_task_t **tasks,
                                   int count);

// =============================================================================
// Coroutine Sleep
// =============================================================================

/**
 * @brief Sleep for specified milliseconds (coroutine-aware).
 *
 * Suspends the current coroutine for the given duration without blocking
 * the event loop. Other coroutines and I/O operations continue normally.
 *
 * @note **Must be called from within a coroutine** (asserted in debug builds).
 *
 * @code
 *   coro_sleep(ctx, 1000);  // Sleep for 1 second
 * @endcode
 *
 * @param ctx  Event-loop context
 * @param ms   Milliseconds to sleep (0 = yield to scheduler)
 */
CXX_C_API void coro_sleep(coro_context_t *ctx, uint64_t ms);

/* ── Error codes ──────────────────────────────────────────────
 * Values match libuv on the target platform so internal code can
 * use UV_* and TURBO_* interchangeably. A _Static_assert in
 * coro_context.c fires at build time if any value drifts.
 * ──────────────────────────────────────────────────────────── */
#define TURBO_OK 0
#define TURBO_EOF (-4095)

#ifdef _WIN32
  #define TURBO_ENOMEM (-4057)
  #define TURBO_EINVAL (-4071)
  #define TURBO_ETIMEDOUT (-4039)
  #define TURBO_ECONNREFUSED (-4078)
  #define TURBO_EPROTONOSUPPORT (-4045)
  #define TURBO_EALREADY (-4084)
#else
  /* POSIX: libuv negates errno.h values.  Standard Linux values
   * below; the _Static_assert guards will catch any mismatch. */
  #define TURBO_ENOMEM (-12)
  #define TURBO_EINVAL (-22)
  #define TURBO_ETIMEDOUT (-110)
  #define TURBO_ECONNREFUSED (-111)
  #define TURBO_EPROTONOSUPPORT (-93)
  #define TURBO_EALREADY (-114)
#endif

// =============================================================================
// Memory Pool
// =============================================================================

/**
 * @brief Get the global memory pool for CoroNet connections.
 *
 * All connections (TCP/UDP/KCP/TLS/WS/Pipe) MUST allocate memory from this
 * pool. This ensures unified memory management and reduces fragmentation.
 *
 * The pool is thread-safe and lazily initialized on first access.
 *
 * @return Pointer to the global memory pool (never NULL)
 */
CXX_C_API void* coro_get_memory_pool(void);

#ifdef __cplusplus
}
#endif

#endif /* coro_CONTEXT_H */
