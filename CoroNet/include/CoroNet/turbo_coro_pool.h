/**
 * @file turbo_coro_pool.h
 * @brief Coroutine object pool for high-performance coroutine reuse.
 *
 * DESIGN PHILOSOPHY (Linus-style "good taste"):
 * - Reuse coro_t objects to avoid malloc/free overhead
 * - Free-list based allocation (O(1) acquire/release)
 * - Single-threaded (no locks needed in event loop)
 * - Hidden from end users (integrated into coro_context)
 *
 * USAGE SCENARIO:
 * - High-concurrency servers (1000+ connections/sec)
 * - Each connection spawns a handler coroutine
 * - Reuse coroutine objects instead of creating new ones
 *
 * PERFORMANCE:
 * - Allocation: O(1) - pop from free-list
 * - Deallocation: O(1) - push to free-list
 * - 10-100x faster than malloc/free for coroutine objects
 */

#ifndef TURBO_CORO_POOL_H
#define TURBO_CORO_POOL_H

#include "platform.h"
#include "turbo_coro.h"
#include "turbo_coro_context.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque coroutine object pool handle */
typedef struct coro_object_pool_s coro_object_pool_t;

/** Pool configuration */
typedef struct {
    size_t initial_capacity;  /**< Initial number of coroutines to pre-allocate */
    size_t max_capacity;      /**< Maximum capacity (0 = unlimited) */
    size_t stack_size;        /**< Stack size per coroutine (0 = default 56KB) */
} coro_object_pool_config_t;

/** Default configuration */
#define CORO_OBJECT_POOL_CONFIG_DEFAULT { 16, 1024, 0 }

/* ── Lifecycle ─────────────────────────────────────────────── */

/**
 * @brief Create a coroutine object pool
 * @param config Pool configuration (NULL for defaults)
 * @param ctx Coroutine context (required for arena allocation)
 * @return Pool handle or NULL on failure
 */
CXX_C_API coro_object_pool_t *coro_object_pool_create(const coro_object_pool_config_t *config,
                                                       coro_context_t *ctx);

/**
 * @brief Destroy the pool and free all coroutines
 * @param pool Pool handle (NULL-safe)
 *
 * WARNING: All coroutines must be released before destroying the pool.
 */
CXX_C_API void coro_object_pool_destroy(coro_object_pool_t *pool);

/* ── Acquire / Release ────────────────────────────────────── */

/**
 * @brief Acquire a coroutine from the pool
 * @param pool Pool handle
 * @param fn Entry function for the coroutine
 * @param arg Argument passed to entry function
 * @return Coroutine handle or NULL if pool is at max capacity
 *
 * PERFORMANCE: O(1) - pop from free-list or allocate new
 */
CXX_C_API coro_t *coro_object_pool_acquire(coro_object_pool_t *pool, coro_fn fn, void *arg);

/**
 * @brief Release a coroutine back to the pool
 * @param pool Pool handle
 * @param co Coroutine to release (must have been acquired from this pool)
 *
 * PERFORMANCE: O(1) - push to free-list
 * NOTE: The coroutine must be in DEAD state before releasing
 */
CXX_C_API void coro_object_pool_release(coro_object_pool_t *pool, coro_t *co);

/* ── Query ────────────────────────────────────────────────── */

/**
 * @brief Get number of free coroutines in the pool
 * @param pool Pool handle
 * @return Number of available coroutines
 */
CXX_C_API size_t coro_object_pool_free_count(const coro_object_pool_t *pool);

/**
 * @brief Get number of active (acquired) coroutines
 * @param pool Pool handle
 * @return Number of coroutines currently in use
 */
CXX_C_API size_t coro_object_pool_active_count(const coro_object_pool_t *pool);

/**
 * @brief Get total capacity of the pool
 * @param pool Pool handle
 * @return Total number of coroutines (free + active)
 */
CXX_C_API size_t coro_object_pool_capacity(const coro_object_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CORO_POOL_H */
