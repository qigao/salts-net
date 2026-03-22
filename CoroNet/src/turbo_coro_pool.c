/**
 * @file turbo_coro_pool.c
 * @brief Coroutine object pool implementation.
 *
 * DESIGN:
 * - Free-list of pre-allocated coroutines
 * - When acquire: pop from free-list or create new
 * - When release: reset coroutine state and push to free-list
 * - Single-threaded (no locks needed)
 *
 * MEMORY LAYOUT:
 * - Each coroutine is wrapped in a pool_entry_t
 * - Free entries are linked in a singly-linked list
 * - Active entries are tracked by count
 */

#include "CoroNet/turbo_coro_pool.h"
#include "turbo_coro.h"
#include "turbo_coro_internal.h"
#include "turbo_buffer.h"
#include <string.h>
#include <stdlib.h>

/* ── Pool entry wrapper ───────────────────────────────────── */

typedef struct pool_entry_s {
    coro_t *coro;                   /**< Coroutine handle */
    struct pool_entry_s *next;      /**< Next free entry (intrusive list) */
} pool_entry_t;

/* ── Pool structure ───────────────────────────────────────── */

struct coro_object_pool_s {
    coro_object_pool_config_t config;
    pool_entry_t *free_list;        /**< Head of free-list */
    size_t free_count;              /**< Number of free coroutines */
    size_t active_count;            /**< Number of active coroutines */
    size_t total_capacity;          /**< Total allocated coroutines */
    mem_pool_t* arena;             /**< Arena for entry shells */
};

/* ── Forward declarations ─────────────────────────────────── */

static pool_entry_t *create_entry(coro_object_pool_t *pool);

/* ── Lifecycle ────────────────────────────────────────────── */

coro_object_pool_t *coro_object_pool_create(const coro_object_pool_config_t *config,
                                            coro_context_t *ctx) {
    coro_object_pool_config_t defaults = CORO_OBJECT_POOL_CONFIG_DEFAULT;
    if (!config) config = &defaults;
    if (!ctx || !ctx->arena) return NULL;

    coro_object_pool_t *pool = (coro_object_pool_t *)calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->config = *config;
    pool->free_list = NULL;
    pool->free_count = 0;
    pool->active_count = 0;
    pool->total_capacity = 0;

    /* Use context's arena for entry shells */
    pool->arena = ctx->arena;

    /* Pre-allocate initial_capacity entry shells (coroutines created on-demand) */
    for (size_t i = 0; i < config->initial_capacity; i++) {
        pool_entry_t *entry = create_entry(pool);
        if (!entry) {
            coro_object_pool_destroy(pool);
            return NULL;
        }
        entry->next = pool->free_list;
        pool->free_list = entry;
        pool->free_count++;
        pool->total_capacity++;
    }

    return pool;
}

void coro_object_pool_destroy(coro_object_pool_t *pool) {
    if (!pool) return;
    assert(pool->active_count == 0 && "destroying coro pool with active coroutines");

    /* Destroy all entries in free-list */
    pool_entry_t *entry = pool->free_list;
    while (entry) {
        pool_entry_t *next = entry->next;
        /* Coroutines are externally allocated, must destroy them */
        if (entry->coro) coro_destroy(entry->coro);
        entry = next;
    }

    
    free(pool);
}

/* ── Acquire / Release ────────────────────────────────────── */

coro_t *coro_object_pool_acquire(coro_object_pool_t *pool, coro_fn fn, void *arg) {
    if (!pool || !fn) return NULL;

    pool_entry_t *entry = NULL;

    if (pool->free_list) {
        entry = pool->free_list;
        pool->free_list = entry->next;
        pool->free_count--;
    } else {
        if (pool->config.max_capacity > 0 && pool->total_capacity >= pool->config.max_capacity) {
            return NULL;
        }
        entry = create_entry(pool);
        if (!entry) return NULL;
        pool->total_capacity++;
    }

    if (!entry->coro) {
        coro_opts_t opts = {
            .stack_size = pool->config.stack_size,
            .storage_size = 0,
            .user_data = NULL
        };
        entry->coro = coro_create(fn, arg, &opts);
        if (!entry->coro) {
            entry->next = pool->free_list;
            pool->free_list = entry;
            pool->free_count++;
            return NULL;
        }
        coro_set_data(entry->coro, entry);
    } else {
        if (coro_reset(entry->coro, fn, arg) != 0) {
            entry->next = pool->free_list;
            pool->free_list = entry;
            pool->free_count++;
            return NULL;
        }
    }

    pool->active_count++;
    return entry->coro;
}

void coro_object_pool_release(coro_object_pool_t *pool, coro_t *co) {
    if (!pool || !co) return;

    /* Verify coroutine is dead before releasing */
    if (coro_state(co) != coro_DEAD) {
        return; /* Cannot release a live coroutine */
    }

    /* Get entry from back-pointer */
    pool_entry_t *entry = (pool_entry_t *)coro_get_data(co);
    if (!entry) {
        /* Coroutine not from a pool or no back-pointer, destroy it */
        coro_destroy(co);
        return;
    }

    entry->next = pool->free_list;
    pool->free_list = entry;
    pool->free_count++;

    if (pool->active_count > 0) {
        pool->active_count--;
    }
}

void coro_object_pool_forget_active(coro_object_pool_t *pool) {
    if (!pool) {
        return;
    }

    pool->active_count = 0;
}

/* ── Query ────────────────────────────────────────────────── */

size_t coro_object_pool_free_count(const coro_object_pool_t *pool) {
    return pool ? pool->free_count : 0;
}

size_t coro_object_pool_active_count(const coro_object_pool_t *pool) {
    return pool ? pool->active_count : 0;
}

size_t coro_object_pool_capacity(const coro_object_pool_t *pool) {
    return pool ? pool->total_capacity : 0;
}

/* ── Internal helpers ─────────────────────────────────────── */

static pool_entry_t *create_entry(coro_object_pool_t *pool) {
    pool_entry_t *entry = (pool_entry_t *)mem_alloc(pool->arena, sizeof(pool_entry_t));
    if (!entry) return NULL;
    entry->coro = NULL;
    entry->next = NULL;
    return entry;
}
