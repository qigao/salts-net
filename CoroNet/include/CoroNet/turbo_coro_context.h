/**
 * @file coro_context.h
 * @brief Opaque event-loop context for coroutine-based networking.
 *
 * Users create a context, pass it to client/server constructors,
 * and run/stop it. No libuv types leak into the public API.
 */

#ifndef coro_CONTEXT_H
#define coro_CONTEXT_H


#include "coronet_api.h"
#include "platform.h"
#include "turbo_backend.h"
#include "turbo_error.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/** Opaque event-loop context (wraps the active native event loop plus a thread-safe post queue) */
typedef struct coro_context_s coro_context_t;

/** Default capacity of each of the two receive buffers owned by a new stream. */
#define CORO_CONTEXT_DEFAULT_STREAM_RECV_BUFFER_SIZE (128u * 1024u)

/** Opaque reusable coroutine-aware timed wait handle. */
typedef struct coro_wait_s coro_wait_t;

/** Shorter alias for coro_context_t */
typedef struct coro_context_s coro_context;

/** Pool configuration defined by turbo_coro_object_pool.h. */
struct coro_object_pool_config_s;

/** Opaque native loop handle (replaces uv_loop_t) */
typedef struct turbo_loop_s turbo_loop_t;

/** Native loop basic lifecycle APIs (called internally by coro_context.c) */
CORONET_C_API turbo_loop_t *turbo_loop_create(void);
CORONET_C_API void turbo_loop_destroy(turbo_loop_t *loop);
CORONET_C_API void turbo_loop_stop(turbo_loop_t *loop);
CORONET_C_API void turbo_loop_poll(turbo_loop_t *loop, int max_ms, int block);
CORONET_C_API int turbo_loop_alive(turbo_loop_t *loop);
CORONET_C_API uint64_t turbo_loop_now(turbo_loop_t *loop);
CORONET_C_API void turbo_loop_ref(turbo_loop_t *loop);
CORONET_C_API void turbo_loop_unref(turbo_loop_t *loop);
CORONET_C_API void turbo_loop_wake(turbo_loop_t *loop);

/**
 * @brief Return the backend loop object wrapped by this context.
 *
 * The pointer is intentionally opaque. New code should prefer context-aware
 * APIs and only use this as a bridge while migrating away from direct loop
 * access.
 *
 * @param ctx Context to query
 * @return Opaque native/backend loop pointer, or NULL for NULL context
 */
CORONET_C_API void *coro_context_native_loop(const coro_context_t *ctx);

/**
 * @brief Create an event-loop context.
 *
 * @param loop  Existing native event-loop pointer to wrap, or NULL to allocate
 *              and own a fresh loop for the current platform backend.
 *
 * When @p loop is NULL the context allocates its own loop and closes it on
 * coro_context_destroy().  When @p loop is non-NULL the caller retains
 * ownership — the context will NOT close or free it on destroy.
 *
 * @return Context handle or NULL on failure
 */
CORONET_C_API coro_context_t *coro_context_create(void *loop);

/**
 * @brief Create an event-loop context with an explicitly sized coroutine pool.
 *
 * The configuration is copied while the context is created and applies to the
 * context-owned scheduler used by coro_context_spawn(), socket listeners, and
 * task APIs. Include turbo_coro_object_pool.h to construct the configuration.
 * Passing NULL preserves coro_context_create() defaults.
 *
 * @param loop Existing native event-loop pointer, or NULL for an owned loop.
 * @param pool_config Coroutine pool capacity and stack configuration, or NULL.
 * @return Context handle or NULL when configuration or allocation fails.
 */
CORONET_C_API coro_context_t *
coro_context_create_ex(void *loop, const struct coro_object_pool_config_s *pool_config);

/**
 * @brief Destroy the context and free resources.
 *
 * If the context owns the loop (created with loop=NULL), the loop is
 * closed and freed. If wrapping an existing loop, only the
 * context struct is freed.
 *
 * @param ctx  Context to destroy (NULL-safe)
 */
CORONET_C_API void coro_context_destroy(coro_context_t *ctx);

/**
 * Keep a context alive while an external asynchronous producer may post work.
 * Calls are thread-safe and must be balanced with
 * coro_context_release_external(). The caller must release the reference
 * before destroying its producer.
 */
CORONET_C_API void coro_context_acquire_external(coro_context_t *ctx);

/** Release one external-producer reference acquired for this context. */
CORONET_C_API void coro_context_release_external(coro_context_t *ctx);

/**
 * @brief Controls how coro_context_run() drives the event loop.
 *
 * Values remain stable across backends so the implementation can forward
 * them directly to the active loop driver.
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
CORONET_C_API int coro_context_run(coro_context_t *ctx, turbo_run_mode_t mode);

/**
 * @brief Stop the event loop.
 *
 * Causes a running coro_context_run() to return on the
 * next iteration. Can be called from any thread.
 *
 * @param ctx  Context to stop
 */
CORONET_C_API void coro_context_stop(coro_context_t *ctx);

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
CORONET_C_API void coro_context_set_persistent(coro_context_t *ctx, int persistent);

/**
 * @brief Set the preferred TCP backend for coroutine sockets created by this context.
 *
 * Unsupported backends fail loudly. Existing sockets keep their current backend;
 * only future TCP sockets created through this context use the new preference.
 *
 * @param ctx      Context to modify
 * @param backend  Preferred backend
 * @return 0 on success, TURBO_EPROTONOSUPPORT when the backend was not built
 *         or cannot be initialized in the current runtime environment, or
 *         another negative error code on failure
 */
CORONET_C_API int coro_context_set_tcp_backend(coro_context_t *ctx, turbo_tcp_backend_t backend);

/**
 * @brief Get the preferred TCP backend for future coroutine TCP sockets.
 *
 * @param ctx Context to query
 * @return Preferred backend, or AUTO for NULL
 */
CORONET_C_API turbo_tcp_backend_t coro_context_get_tcp_backend(const coro_context_t *ctx);

/**
 * @brief Set the preferred UDP backend for coroutine sockets created by this context.
 *
 * Unsupported backends fail loudly. Existing sockets keep their current backend;
 * only future UDP sockets created through this context use the new preference.
 *
 * @param ctx      Context to modify
 * @param backend  Preferred backend
 * @return 0 on success, negative error code on failure
 */
CORONET_C_API int coro_context_set_udp_backend(coro_context_t *ctx, turbo_udp_backend_t backend);

/**
 * @brief Get the preferred UDP backend for future coroutine UDP sockets.
 *
 * @param ctx Context to query
 * @return Preferred backend, or AUTO for NULL
 */
CORONET_C_API turbo_udp_backend_t coro_context_get_udp_backend(const coro_context_t *ctx);

/**
 * @brief Set the receive-buffer capacity used by streams created after this call.
 *
 * Each stream owns two ping-pong buffers of this capacity. The setting is
 * context-local and does not resize streams that already exist. Call it from
 * the context owner lane before creating sockets or streams.
 *
 * @param ctx Context to configure.
 * @param bytes Non-zero capacity of each receive buffer.
 * @return TURBO_OK or TURBO_EINVAL.
 */
CORONET_C_API int coro_context_set_stream_recv_buffer_size(coro_context_t *ctx, size_t bytes);

/** @return Configured per-buffer capacity, or the default for a NULL context. */
CORONET_C_API size_t coro_context_get_stream_recv_buffer_size(const coro_context_t *ctx);

/**
 * @brief Get the last synchronous API error recorded on this context.
 *
 * Stream/listener factory helpers that return NULL store their failure code on
 * the owning context so callers can inspect the real cause instead of guessing.
 *
 * @param ctx Context to query
 * @return 0 if no error is recorded, or the last negative TURBO/system error
 */
CORONET_C_API int coro_context_get_last_error(const coro_context_t *ctx);

// =============================================================================
// Query
// =============================================================================

/**
 * @brief Check if the event loop has active handles or requests.
 * @param ctx  Context to query
 * @return 1 if alive (has work to do), 0 if idle
 */
CORONET_C_API int coro_context_alive(coro_context_t *ctx);

/**
 * @brief Get the number of active coroutines in this context.
 * @param ctx  Context to query
 * @return Count of alive coroutines
 */
CORONET_C_API int coro_context_coro_count(coro_context_t *ctx);

/**
 * @brief Get the cached event-loop timestamp (milliseconds).
 *
 * Updated once per loop iteration — zero syscall overhead.
 * Useful for timeouts, rate limiting, and relative timing.
 *
 * @param ctx  Context to query
 * @return Monotonic time in milliseconds
 */
CORONET_C_API uint64_t coro_context_now(coro_context_t *ctx);

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
CORONET_C_API int coro_post(coro_context_t *ctx, coro_post_fn fn, void *arg1, void *arg2);

/**
 * @brief Get the current thread's event-loop context.
 *
 * This allows components to implicitly find the event loop they are running on,
 * avoiding the need to pass the context explicitly everywhere.
 *
 * @return Current context or NULL if no context is active on this thread.
 */
CORONET_C_API coro_context_t *coro_context_current(void);

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
CORONET_C_API int coro_context_spawn(coro_context_t *ctx, coro_fn fn, void *arg);

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
 * The task handle remains valid after completion so callers can inspect
 * completion state or combine it with other tasks. Destroy it explicitly
 * with coro_task_destroy(), or let coro_context_destroy() reclaim it.
 *
 * @param ctx  Event-loop context
 * @param fn   Coroutine entry function
 * @param arg  Argument passed to fn
 * @return Task handle or NULL on failure
 */
CORONET_C_API coro_task_t *coro_task_create(coro_context_t *ctx,
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
CORONET_C_API int coro_task_start(coro_task_t *task);

/**
 * @brief Cancel a lazy task.
 *
 * Cancels a task that has not yet been started. Once cancelled, the task
 * cannot be started and becomes "done".
 *
 * @param task  Task to cancel
 * @return 0 on success, TURBO_EINVAL if already started
 */
CORONET_C_API int coro_task_cancel(coro_task_t *task);

/**
 * @brief Check if a task has completed.
 *
 * @param task  Task to check
 * @return 1 if completed, 0 if still running or not started
 */
CORONET_C_API int coro_task_is_done(coro_task_t *task);

/**
 * @brief Manually destroy a task.
 *
 * Releases the caller's ownership of a task handle. Unstarted tasks are
 * destroyed immediately; completed tasks are destroyed once no references
 * remain. Any task still owned by the context is reclaimed on
 * coro_context_destroy().
 *
 * @param task  Task to destroy (NULL-safe)
 */
CORONET_C_API void coro_task_destroy(coro_task_t *task);

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
CORONET_C_API int coro_when_all(coro_context_t *ctx,
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
CORONET_C_API int coro_when_any(coro_context_t *ctx,
                                   coro_task_t **tasks,
                                   int count);

// =============================================================================
// Coroutine Sleep
// =============================================================================

/** Create a reusable wait bound to @p ctx. One handle supports one active waiter. */
CORONET_C_API coro_wait_t *coro_wait_create(coro_context_t *ctx);

/** Destroy an idle wait. Returns TURBO_EBUSY while a wait is active. */
CORONET_C_API int coro_wait_destroy(coro_wait_t *wait);

/**
 * Suspend the current coroutine without blocking the event loop.
 * Returns TURBO_OK after @p ms, or the status passed to coro_wait_interrupt().
 */
CORONET_C_API int coro_wait_for(coro_wait_t *wait, uint64_t ms);

/**
 * Interrupt an active wait from any thread. Exactly one timer or interrupt
 * completion resumes the waiter. @p status must be a non-zero Turbo error.
 */
CORONET_C_API int coro_wait_interrupt(coro_wait_t *wait, int status);

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
CORONET_C_API void coro_sleep(coro_context_t *ctx, uint64_t ms);

/* ── Error codes ──────────────────────────────────────────────
 * Defined in turbo_error.h — included here for convenience.
 * ──────────────────────────────────────────────────────────── */
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
CORONET_C_API void* coro_get_memory_pool(void);
CORONET_C_API void* coro_context_get_arena(coro_context_t *ctx);
CORONET_C_API void coro_context_native_ref(coro_context_t *ctx);
CORONET_C_API void coro_context_native_unref(coro_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* coro_CONTEXT_H */
