/**
 * @file turbo_coro_thread_pool.h
 * @brief Thread pool of coroutine contexts for multi-core execution.
 */

#ifndef TURBO_CORO_THREAD_POOL_H
#define TURBO_CORO_THREAD_POOL_H

#include "turbo_coro_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coro_thread_pool_s coro_thread_pool_t;

/**
 * @brief Create a coroutine thread pool.
 * 
 * Each thread in the pool runs its own event loop and coro_context.
 * 
 * @param num_threads Number of threads (0 = auto-detect CPU count)
 * @return Pool handle or NULL on failure
 */
CXX_C_API coro_thread_pool_t *coro_thread_pool_create(int num_threads);

/**
 * @brief Destroy the thread pool and stop all threads.
 * @param pool Pool handle
 */
CXX_C_API void coro_thread_pool_destroy(coro_thread_pool_t *pool);

/**
 * @brief Spawn a coroutine on the thread pool (Round-robin distribution).
 * 
 * @param pool Thread pool
 * @param fn   Coroutine entry function
 * @param arg  User argument
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int coro_thread_pool_spawn(coro_thread_pool_t *pool, coro_fn fn, void *arg);

/**
 * @brief Get a specific context from the pool (e.g. for affinity).
 * @param pool  Thread pool
 * @param index Thread index
 * @return Context handle
 */
CXX_C_API coro_context_t *coro_thread_pool_get_context(coro_thread_pool_t *pool, int index);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CORO_THREAD_POOL_H */
