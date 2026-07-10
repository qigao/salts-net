/**
 * @file turbo_coro_pool.h
 * @brief Generic coroutine reuse pool for TurboNet Utils.
 */

#ifndef TURBO_UTILS_CORO_POOL_H
#define TURBO_UTILS_CORO_POOL_H

#include "platform.h"
#include "turbo_coro.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_coro_pool_s turbo_coro_pool_t;

typedef void *(*turbo_coro_pool_alloc_fn)(void *user_data, size_t size);
typedef void (*turbo_coro_pool_free_fn)(void *user_data, void *ptr);

typedef struct {
    size_t initial_capacity;             /**< Initial free entries to create */
    size_t max_capacity;                 /**< Maximum entries, 0 = unlimited */
    size_t stack_size;                   /**< Coroutine stack size, 0 = default */
    size_t storage_size;                 /**< Coroutine storage size, 0 = default */
    turbo_coro_pool_alloc_fn alloc_fn;   /**< Optional entry-shell allocator */
    turbo_coro_pool_free_fn free_fn;     /**< Optional entry-shell freer */
    void *allocator_data;                /**< Allocator user data */
} turbo_coro_pool_config_t;

#define TURBO_CORO_POOL_CONFIG_DEFAULT { 16, 1024, 0, 0, NULL, NULL, NULL }

CXX_C_API turbo_coro_pool_t *turbo_coro_pool_create(const turbo_coro_pool_config_t *config);
CXX_C_API void turbo_coro_pool_destroy(turbo_coro_pool_t *pool);

CXX_C_API coro_t *turbo_coro_pool_acquire(turbo_coro_pool_t *pool, coro_fn fn, void *arg);
CXX_C_API void turbo_coro_pool_release(turbo_coro_pool_t *pool, coro_t *co);

/**
 * @brief Reclaim pool bookkeeping for a coroutine being force-destroyed.
 *
 * This is normally called through the coroutine discard hook registered by
 * turbo_coro_pool_acquire().
 */
CXX_C_API void turbo_coro_pool_discard_coro(coro_t *co);

CXX_C_API void turbo_coro_pool_forget_active(turbo_coro_pool_t *pool);

CXX_C_API coro_t *turbo_coro_spawn_pooled(coro_scheduler_t *sched,
                                          turbo_coro_pool_t *pool,
                                          coro_fn fn,
                                          void *arg);

CXX_C_API size_t turbo_coro_pool_free_count(const turbo_coro_pool_t *pool);
CXX_C_API size_t turbo_coro_pool_active_count(const turbo_coro_pool_t *pool);
CXX_C_API size_t turbo_coro_pool_capacity(const turbo_coro_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_UTILS_CORO_POOL_H */
