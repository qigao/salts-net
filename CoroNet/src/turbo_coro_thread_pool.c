#include "CoroNet/turbo_coro_thread_pool.h"
#include "CoroNet/turbo_coro_pool.h"
#include "turbo_coro_internal.h"
#include "turbo_thread.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <uv.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

#define UNUSED(x) (void)(x)

struct coro_thread_pool_s {
    coro_context_t **contexts;
    turbo_thread_t *threads;
    int thread_count;
    atomic_int next_thread;
    volatile int stopping;
};

/* ── Worker Logic ─────────────────────────────────────────── */

typedef struct {
    coro_thread_pool_t *pool;
    int index;
} worker_ctx_t;

static void worker_thread_refined(void *arg) {
    worker_ctx_t *wctx = (worker_ctx_t *)arg;
    coro_thread_pool_t *pool = wctx->pool;
    coro_context_t *ctx = pool->contexts[wctx->index];
    free(wctx);

    /* Good taste: Use TURBO_RUN_DEFAULT to sleep efficiently in the kernel 
     * and wake up immediately on coro_post (uv_async_send). */
    while (!pool->stopping) {
        coro_context_run(ctx, TURBO_RUN_DEFAULT);
    }
}

coro_thread_pool_t *coro_thread_pool_create(int num_threads) {
    if (num_threads <= 0) {
        uv_cpu_info_t *info;
        int count;
        if (uv_cpu_info(&info, &count) == 0) {
            num_threads = count;
            uv_free_cpu_info(info, count);
        } else {
            num_threads = 4;
        }
    }

    coro_thread_pool_t *pool = (coro_thread_pool_t *)calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->thread_count = num_threads;
    atomic_store_explicit(&pool->next_thread, 0, memory_order_relaxed);

    pool->contexts = (coro_context_t **)calloc(num_threads, sizeof(coro_context_t *));
    pool->threads = (turbo_thread_t *)calloc(num_threads, sizeof(turbo_thread_t));

    if (!pool->contexts || !pool->threads) {
        coro_thread_pool_destroy(pool);
        return NULL;
    }

    for (int i = 0; i < num_threads; i++) {
        pool->contexts[i] = coro_context_create(NULL);
        if (!pool->contexts[i]) {
            coro_thread_pool_destroy(pool);
            return NULL;
        }

        /* Good taste: Configure larger object pool for high-throughput scenarios.
         * Default pool (16 initial, 1024 max) is too small for burst workloads.
         * Pre-allocate enough coroutines to handle typical burst without dynamic allocation. */
        coro_object_pool_config_t pool_cfg = {
            .initial_capacity = 256,   /* Pre-allocate 256 coroutines per thread */
            .max_capacity = 4096,      /* Allow up to 4096 per thread (16k total for 4 threads) */
            .stack_size = 0            /* Use default stack size */
        };

        /* Replace the default pool with our optimized one */
        if (pool->contexts[i]->pool) {
            coro_object_pool_destroy(pool->contexts[i]->pool);
        }
        pool->contexts[i]->pool = coro_object_pool_create(&pool_cfg, pool->contexts[i]);
        if (!pool->contexts[i]->pool) {
            coro_thread_pool_destroy(pool);
            return NULL;
        }

        /* Good taste: Workers need to stay alive to wait for posts */
        coro_context_set_persistent(pool->contexts[i], 1);

        worker_ctx_t *wctx = malloc(sizeof(worker_ctx_t));
        wctx->pool = pool;
        wctx->index = i;

        if (turbo_thread_create(&pool->threads[i], worker_thread_refined, wctx) != 0) {
            free(wctx);
            coro_thread_pool_destroy(pool);
            return NULL;
        }
    }

    return pool;
}

void coro_thread_pool_destroy(coro_thread_pool_t *pool) {
    if (!pool) return;

    pool->stopping = 1;

    /* 1. Wake up all loops so they can see the stopping flag */
    for (int i = 0; i < pool->thread_count; i++) {
        if (pool->contexts[i]) {
            coro_context_stop(pool->contexts[i]);
        }
    }

    /* 2. Join threads */
    for (int i = 0; i < pool->thread_count; i++) {
        if (pool->threads[i]) {
            turbo_thread_join(&pool->threads[i]);
        }
    }

    /* 3. Cleanup contexts */
    for (int i = 0; i < pool->thread_count; i++) {
        if (pool->contexts[i]) {
            coro_context_destroy(pool->contexts[i]);
        }
    }

    free(pool->contexts);
    free(pool->threads);
    
    free(pool);
}

static void spawn_proxy(void *arg1, void *arg2) {
    coro_fn fn = (coro_fn)arg1;
    void *arg = arg2;
    coro_context_spawn(coro_context_current(), fn, arg);
}

int coro_thread_pool_spawn(coro_thread_pool_t *pool, coro_fn fn, void *arg) {
    if (!pool || !fn || pool->stopping) return -1;

    /* Use atomic increment for fair distribution */
    int idx = atomic_fetch_add(&pool->next_thread, 1) % pool->thread_count;
    coro_context_t *target_ctx = pool->contexts[idx];

    /* Good taste: Passes fn and arg directly through lock-free MPSC queue, zero allocations! */
    return coro_post(target_ctx, spawn_proxy, (void*)fn, arg);
}

coro_context_t *coro_thread_pool_get_context(coro_thread_pool_t *pool, int index) {
    if (!pool || index < 0 || index >= pool->thread_count) return NULL;
    return pool->contexts[index];
}
