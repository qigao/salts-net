/**
 * @file turbo_coro_pool.c
 * @brief CoroNet coroutine object pool adapter.
 */

#include "CoroNet/turbo_coro_object_pool.h"
#include <turbo_coro_pool.h>
#include "turbo_buffer.h"
#include "turbo_coro_internal.h"
#include <assert.h>
#include <stdlib.h>

struct coro_object_pool_s {
    turbo_coro_pool_t *pool;
    mem_pool_t *arena;
};

static void *coro_pool_arena_alloc(void *user_data, size_t size) {
    return mem_alloc((mem_pool_t *)user_data, size);
}

coro_object_pool_t *coro_object_pool_create(const coro_object_pool_config_t *config,
                                            coro_context_t *ctx) {
    coro_object_pool_config_t defaults = CORO_OBJECT_POOL_CONFIG_DEFAULT;
    turbo_coro_pool_config_t pool_config = TURBO_CORO_POOL_CONFIG_DEFAULT;
    coro_object_pool_t *pool = NULL;

    if (!config) config = &defaults;
    if (!ctx || !ctx->arena) return NULL;
    if (config->max_capacity != 0u && config->initial_capacity > config->max_capacity) return NULL;

    pool = (coro_object_pool_t *)calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool_config.initial_capacity = config->initial_capacity;
    pool_config.max_capacity = config->max_capacity;
    pool_config.stack_size = config->stack_size;
    pool_config.alloc_fn = coro_pool_arena_alloc;
    pool_config.free_fn = NULL;
    pool_config.allocator_data = ctx->arena;

    pool->arena = ctx->arena;
    pool->pool = turbo_coro_pool_create(&pool_config);
    if (!pool->pool) {
        free(pool);
        return NULL;
    }

    return pool;
}

void coro_object_pool_destroy(coro_object_pool_t *pool) {
    if (!pool) return;
    assert(coro_object_pool_active_count(pool) == 0 && "destroying coro pool with active coroutines");
    turbo_coro_pool_destroy(pool->pool);
    free(pool);
}

coro_t *coro_object_pool_acquire(coro_object_pool_t *pool, coro_fn fn, void *arg) {
    return pool ? turbo_coro_pool_acquire(pool->pool, fn, arg) : NULL;
}

coro_t *coro_spawn_pooled(coro_scheduler_t *sched,
                          coro_object_pool_t *pool,
                          coro_fn fn,
                          void *arg) {
    return pool ? turbo_coro_spawn_pooled(sched, pool->pool, fn, arg) : NULL;
}

void coro_object_pool_release(coro_object_pool_t *pool, coro_t *co) {
    if (pool) {
        turbo_coro_pool_release(pool->pool, co);
    }
}

void coro_object_pool_discard_coro(coro_t *co) {
    turbo_coro_pool_discard_coro(co);
}

void coro_object_pool_forget_active(coro_object_pool_t *pool) {
    if (pool) {
        turbo_coro_pool_forget_active(pool->pool);
    }
}

size_t coro_object_pool_free_count(const coro_object_pool_t *pool) {
    return pool ? turbo_coro_pool_free_count(pool->pool) : 0;
}

size_t coro_object_pool_active_count(const coro_object_pool_t *pool) {
    return pool ? turbo_coro_pool_active_count(pool->pool) : 0;
}

size_t coro_object_pool_capacity(const coro_object_pool_t *pool) {
    return pool ? turbo_coro_pool_capacity(pool->pool) : 0;
}
