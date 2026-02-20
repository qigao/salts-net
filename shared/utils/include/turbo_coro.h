/**
 * @file turbo_coro.h
 * @brief Lightweight coroutine support for TurboNet
 *
 * Wraps minicoro for stackful asymmetric coroutines.
 * Use for cooperative concurrency without OS threads.
 */

#ifndef TURBO_CORO_H
#define TURBO_CORO_H

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Coroutine handle (opaque) */
typedef struct turbo_coro_s turbo_coro_t;

/** Coroutine entry function */
typedef void (*turbo_coro_fn)(turbo_coro_t *co, void *arg);

/** Coroutine state */
typedef enum {
    TURBO_CORO_DEAD = 0,      /**< Finished or never started */
    TURBO_CORO_READY,         /**< Created, waiting to run */
    TURBO_CORO_RUNNING,       /**< Currently executing */
    TURBO_CORO_SUSPENDED      /**< Yielded, waiting to resume */
} turbo_coro_state_t;

/** Coroutine creation options */
typedef struct {
    size_t stack_size;        /**< Stack size (0 = default 56KB) */
    size_t storage_size;      /**< Storage buffer size (0 = default 1KB) */
    void *user_data;          /**< User data accessible from coroutine */
} turbo_coro_opts_t;

/** Default options */
#define TURBO_CORO_OPTS_DEFAULT { 0, 0, NULL }

// =============================================================================
// Lifecycle
// =============================================================================

/**
 * @brief Create a coroutine
 * @param fn Entry function
 * @param arg Argument passed to entry function
 * @param opts Options (NULL for defaults)
 * @return Coroutine handle or NULL on failure
 */
CXX_C_API turbo_coro_t *turbo_coro_create(turbo_coro_fn fn, void *arg, const turbo_coro_opts_t *opts);

/**
 * @brief Destroy a coroutine
 * @param co Coroutine to destroy
 */
CXX_C_API void turbo_coro_destroy(turbo_coro_t *co);

// =============================================================================
// Execution Control
// =============================================================================

/**
 * @brief Resume a coroutine (start or continue execution)
 * @param co Coroutine to resume
 * @return 0 on success, -1 on error
 */
CXX_C_API int turbo_coro_resume(turbo_coro_t *co);

/**
 * @brief Yield from current coroutine (suspend execution)
 * @return 0 on success, -1 on error
 *
 * @note Must be called from within a coroutine
 */
CXX_C_API int turbo_coro_yield(void);

/**
 * @brief Get current coroutine state
 * @param co Coroutine to query
 * @return Current state
 */
CXX_C_API turbo_coro_state_t turbo_coro_state(turbo_coro_t *co);

/**
 * @brief Check if coroutine is alive (not dead)
 * @param co Coroutine to check
 * @return 1 if alive, 0 if dead
 */
CXX_C_API int turbo_coro_alive(turbo_coro_t *co);

// =============================================================================
// Context
// =============================================================================

/**
 * @brief Get currently running coroutine
 * @return Current coroutine or NULL if not in a coroutine
 */
CXX_C_API turbo_coro_t *turbo_coro_running(void);

/**
 * @brief Get user data from coroutine
 * @param co Coroutine
 * @return User data pointer
 */
CXX_C_API void *turbo_coro_get_data(turbo_coro_t *co);

/**
 * @brief Set user data on coroutine
 * @param co Coroutine
 * @param data User data pointer
 */
CXX_C_API void turbo_coro_set_data(turbo_coro_t *co, void *data);

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
CXX_C_API int turbo_coro_push(turbo_coro_t *co, const void *data, size_t size);

/**
 * @brief Pop data from coroutine storage
 * @param co Coroutine
 * @param data Buffer to receive data
 * @param size Size in bytes
 * @return 0 on success, -1 on error
 */
CXX_C_API int turbo_coro_pop(turbo_coro_t *co, void *data, size_t size);

/**
 * @brief Get bytes available in storage
 * @param co Coroutine
 * @return Bytes stored
 */
CXX_C_API size_t turbo_coro_bytes_stored(turbo_coro_t *co);

// =============================================================================
// Coroutine Scheduler
// =============================================================================

/** Scheduler handle (opaque) */
typedef struct turbo_coro_scheduler_s turbo_coro_scheduler_t;

/**
 * @brief Create a coroutine scheduler
 * @return Scheduler or NULL on failure
 */
CXX_C_API turbo_coro_scheduler_t *turbo_coro_scheduler_create(void);

/**
 * @brief Destroy scheduler (destroys all managed coroutines)
 * @param sched Scheduler to destroy
 */
CXX_C_API void turbo_coro_scheduler_destroy(turbo_coro_scheduler_t *sched);

/**
 * @brief Spawn a new coroutine in the scheduler
 * @param sched Scheduler
 * @param fn Entry function
 * @param arg Argument passed to entry function
 * @return Coroutine handle or NULL on failure
 */
CXX_C_API turbo_coro_t *turbo_coro_spawn(turbo_coro_scheduler_t *sched, turbo_coro_fn fn, void *arg);

/**
 * @brief Run one scheduling round (resume all ready coroutines once)
 * @param sched Scheduler
 * @return Number of coroutines still alive
 */
CXX_C_API int turbo_coro_scheduler_tick(turbo_coro_scheduler_t *sched);

/**
 * @brief Run until all coroutines complete
 * @param sched Scheduler
 */
CXX_C_API void turbo_coro_scheduler_run(turbo_coro_scheduler_t *sched);

/**
 * @brief Get number of active coroutines
 * @param sched Scheduler
 * @return Number of alive coroutines
 */
CXX_C_API int turbo_coro_scheduler_count(turbo_coro_scheduler_t *sched);

/**
 * @brief Get scheduler from current coroutine
 * @return Scheduler or NULL if not in a scheduled coroutine
 */
CXX_C_API turbo_coro_scheduler_t *turbo_coro_current_scheduler(void);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CORO_H */
