/**
 * @file turbo_coro.h
 * @brief Lightweight coroutine support for TurboNet
 *
 * Wraps minicoro for stackful asymmetric coroutines.
 * Use for cooperative concurrency without OS threads.
 */

#ifndef coro_H
#define coro_H

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Coroutine handle (opaque) */
typedef struct coro_s coro_t;

/** Coroutine entry function */
typedef void (*coro_fn)(coro_t *co, void *arg);

/** Coroutine state */
typedef enum {
    coro_DEAD = 0,      /**< Finished or never started */
    coro_READY,         /**< Created, not yet resumed for the first time */
    coro_RUNNING,       /**< Currently executing on the call stack */
    coro_SUSPENDED      /**< Yielded (or resumed a child) — waiting to run again */
} coro_state_t;

/** Coroutine creation options */
typedef struct {
    size_t stack_size;        /**< Stack size (0 = default 56KB) */
    size_t storage_size;      /**< Storage buffer size (0 = default 1KB) */
    void *user_data;          /**< User data accessible from coroutine */
} coro_opts_t;

/** Default options */
#define coro_OPTS_DEFAULT { 0, 0, NULL }

// =============================================================================
// Lifecycle (Advanced API - Manual Management)
// =============================================================================

/**
 * @brief Create a coroutine (Advanced API)
 *
 * @warning This is a low-level API for advanced use cases only.
 *          Most users should use coro_context_spawn() or coro_task_create()
 *          from coro_context.h, which provide automatic lifecycle management.
 *
 *          Manual management requires:
 *          - Calling coro_resume() to start/continue execution
 *          - Calling coro_destroy() when done
 *          - Careful handling of coroutine state and cleanup
 *
 * @param fn Entry function
 * @param arg Argument passed to entry function
 * @param opts Options (NULL for defaults)
 * @return Coroutine handle or NULL on failure
 */
CXX_C_API coro_t *coro_create(coro_fn fn, void *arg, const coro_opts_t *opts);

/**
 * @brief Destroy a coroutine (Advanced API)
 *
 * @warning Only call this for coroutines created with coro_create().
 *          Do NOT call this for coroutines managed by coro_context_spawn()
 *          or coro_task_create() - they are automatically cleaned up.
 *
 * @param co Coroutine to destroy
 */
CXX_C_API void coro_destroy(coro_t *co);

// =============================================================================
// Execution Control
// =============================================================================

/**
 * @brief Resume a coroutine (start or continue execution) (Advanced API)
 *
 * @warning This is for manually-created coroutines only.
 *          Coroutines spawned via coro_context_spawn() are automatically
 *          resumed by the scheduler.
 *
 * @param co Coroutine to resume
 * @return 0 on success, -1 on error
 */
CXX_C_API int coro_resume(coro_t *co);

/**
 * @brief Yield from current coroutine (suspend execution)
 * @return 0 on success, -1 on error
 *
 * @note Must be called from within a coroutine
 */
CXX_C_API int coro_yield(void);

/**
 * @brief Reset a coroutine for reuse (Advanced API)
 *
 * This allows reusing the coroutine's stack and memory buffer for a new
 * entry function. The coroutine must be in DEAD state.
 *
 * @param co Coroutine to reset
 * @param fn New entry function
 * @param arg New argument
 * @return 0 on success, -1 on error
 */
CXX_C_API int coro_reset(coro_t *co, coro_fn fn, void *arg);

/**
 * @brief Get current coroutine state
 * @param co Coroutine to query
 * @return Current state
 */
CXX_C_API coro_state_t coro_state(coro_t *co);

/**
 * @brief Check if coroutine is alive (not dead)
 * @param co Coroutine to check
 * @return 1 if alive, 0 if dead
 */
CXX_C_API int coro_alive(coro_t *co);

// =============================================================================
// Context
// =============================================================================

/**
 * @brief Get currently running coroutine
 * @return Current coroutine or NULL if not in a coroutine
 */
CXX_C_API coro_t *coro_running(void);

/**
 * @brief Get user data from coroutine
 * @param co Coroutine
 * @return User data pointer
 */
CXX_C_API void *coro_get_data(coro_t *co);

/**
 * @brief Set user data on coroutine
 * @param co Coroutine
 * @param data User data pointer
 */
CXX_C_API void coro_set_data(coro_t *co, void *data);

// =============================================================================
// Data Passing (LIFO storage buffer)
// =============================================================================

/**
 * @brief Push data to coroutine storage
 * @param co Coroutine
 * @param data Data to push
 * @param size Size in bytes
 * @return 0 on success, -1 on error
 */
CXX_C_API int coro_push(coro_t *co, const void *data, size_t size);

/**
 * @brief Pop data from coroutine storage
 * @param co Coroutine
 * @param data Buffer to receive data
 * @param size Size in bytes
 * @return 0 on success, -1 on error
 */
CXX_C_API int coro_pop(coro_t *co, void *data, size_t size);

/**
 * @brief Get bytes available in storage
 * @param co Coroutine
 * @return Bytes stored
 */
CXX_C_API size_t coro_bytes_stored(coro_t *co);

// =============================================================================
// Coroutine Scheduler
// =============================================================================

/** Scheduler handle (opaque) */
typedef struct coro_scheduler_s coro_scheduler_t;

/**
 * @brief Create a coroutine scheduler
 * @return Scheduler or NULL on failure
 */
CXX_C_API coro_scheduler_t *coro_scheduler_create(void);

/**
 * @brief Destroy scheduler
 *
 * @warning All coroutines must be completed before destroying the scheduler.
 *          This function will assert if any coroutines are still alive.
 *          Use coro_scheduler_count() to check before destroying.
 *
 * @param sched Scheduler to destroy
 */
CXX_C_API void coro_scheduler_destroy(coro_scheduler_t *sched);

/**
 * @brief Spawn a new coroutine in the scheduler (lazy start).
 *
 * The coroutine is added to the scheduler's tail and will run on the
 * next scheduler tick — it does NOT execute immediately.
 *
 * Most users should prefer coro_context_spawn() which hooks into
 * the event-loop context, or coro_task_create() + coro_task_start()
 * for explicit control.
 *
 * @param sched Scheduler
 * @param fn    Entry function
 * @param arg   Argument passed to entry function
 * @param opts  Options (NULL for defaults)
 * @return Coroutine handle or NULL on failure
 */
CXX_C_API coro_t *coro_spawn(coro_scheduler_t *sched, coro_fn fn, void *arg, const coro_opts_t *opts);

/**
 * @brief Adopt an existing coroutine into the scheduler.
 * @param sched Scheduler
 * @param co    Coroutine to adopt
 */
CXX_C_API void coro_scheduler_adopt(coro_scheduler_t *sched, coro_t *co);

/**
 * @brief Run one scheduling round (resume all ready coroutines once)
 * @param sched Scheduler
 * @return Number of coroutines still alive
 */
CXX_C_API int coro_scheduler_tick(coro_scheduler_t *sched);

/**
 * @brief Run until all coroutines complete
 * @param sched Scheduler
 */
CXX_C_API void coro_scheduler_run(coro_scheduler_t *sched);

/**
 * @brief Get number of active coroutines
 * @param sched Scheduler
 * @return Number of alive coroutines
 */
CXX_C_API int coro_scheduler_count(coro_scheduler_t *sched);

/**
 * @brief Check if any managed coroutine is ready to run (not blocked on I/O).
 * @param sched Scheduler
 * @return 1 if any coroutine is ready, 0 otherwise
 */
CXX_C_API int coro_scheduler_has_ready(coro_scheduler_t *sched);

/**
 * @brief Get scheduler from current coroutine
 * @return Scheduler or NULL if not in a scheduled coroutine
 */
CXX_C_API coro_scheduler_t *coro_current_scheduler(void);

/**
 * @brief Check if a coroutine is managed by a scheduler
 * @param co Coroutine to check
 * @return 1 if managed by scheduler, 0 if manually managed
 */
CXX_C_API int coro_is_scheduled(coro_t *co);

/**
 * @brief Mark coroutine as waiting for I/O (internal/advanced use only)
 *
 * @warning This is an internal API for integration with I/O event loops.
 *          Incorrect use can cause coroutines to never be scheduled again.
 *          Most users should NOT call this directly.
 *
 * @param co Coroutine
 * @param waiting 1 = waiting for I/O, 0 = ready to run
 */
CXX_C_API void coro_set_waiting_for_io(coro_t *co, int waiting);

/**
 * @brief Set cleanup callback for coroutine
 * @param co Coroutine
 * @param fn Cleanup function
 * @param arg Cleanup argument
 */
CXX_C_API void coro_set_cleanup(coro_t *co, void (*fn)(coro_t *co, void *arg), void *arg);

/**
 * @brief Remove coroutine from scheduler without destroying it.
 * @param co Coroutine to detach
 */
CXX_C_API void coro_detach_scheduler(coro_t *co);

#ifdef __cplusplus
}
#endif

#endif /* coro_H */
