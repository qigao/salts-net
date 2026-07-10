#include "CoroNet/turbo_coro_thread_pool.h"
#include "CoroNet/turbo_coro_object_pool.h"
#include "platform.h"
#include "turbo_coro_internal.h"
#include "turbo_thread.h"
#include <stdatomic.h>
#include <stdlib.h>

#define UNUSED(x) (void)(x)

struct coro_thread_pool_s {
    coro_context_t **contexts;
    turbo_thread_t *threads;
    int thread_count;
    atomic_int next_thread;
    atomic_int stopping;
    atomic_int inflight_spawns;
    turbo_mutex_t lifecycle_mutex;
    turbo_cond_t lifecycle_cond;
};

/* ── Worker Logic ─────────────────────────────────────────── */

typedef struct {
    coro_thread_pool_t *pool;
    int index;
} worker_ctx_t;

static void coro_thread_pool_request_stop_post(void *arg1, void *arg2) {
    (void)arg2;
    coro_context_stop((coro_context_t *)arg1);
}

static void worker_thread_refined(void *arg) {
  worker_ctx_t *wctx = (worker_ctx_t *)arg;
  coro_thread_pool_t *pool = wctx->pool;
  coro_context_t *ctx = pool->contexts[wctx->index];
  uint64_t shutdown_deadline_ms = 0;
  free(wctx);

    /* Good taste: run until shutdown is requested and the context is drained. */
    for (;;) {
        if (!atomic_load_explicit(&pool->stopping, memory_order_acquire)) {
            coro_context_run(ctx, TURBO_RUN_DEFAULT);
            continue;
        }

        if (shutdown_deadline_ms == 0) {
            shutdown_deadline_ms = turbo_uptime_ms() + 8000;
        }

        coro_context_run(ctx, TURBO_RUN_NOWAIT);

        if (!coro_context_alive(ctx) || turbo_uptime_ms() >= shutdown_deadline_ms) {
            break;
        }

        turbo_sleep_ms(1);
    }
}

static void coro_thread_pool_signal_lifecycle(coro_thread_pool_t *pool) {
    if (pool == NULL) {
        return;
    }

    turbo_mutex_lock(&pool->lifecycle_mutex);
    turbo_cond_broadcast(&pool->lifecycle_cond);
    turbo_mutex_unlock(&pool->lifecycle_mutex);
}

static int coro_thread_pool_enter_spawn(coro_thread_pool_t *pool) {
    if (pool == NULL) {
        return -1;
    }

    if (atomic_load_explicit(&pool->stopping, memory_order_acquire)) {
        return -1;
    }

    atomic_fetch_add_explicit(&pool->inflight_spawns, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&pool->stopping, memory_order_acquire)) {
        if (atomic_fetch_sub_explicit(&pool->inflight_spawns, 1, memory_order_acq_rel) == 1) {
            coro_thread_pool_signal_lifecycle(pool);
        }
        return -1;
    }

    return 0;
}

static void coro_thread_pool_leave_spawn(coro_thread_pool_t *pool) {
    if (pool == NULL) {
        return;
    }

    if (atomic_fetch_sub_explicit(&pool->inflight_spawns, 1, memory_order_acq_rel) == 1) {
        coro_thread_pool_signal_lifecycle(pool);
    }
}

coro_thread_pool_t *coro_thread_pool_create(int num_threads) {
    if (num_threads <= 0) {
        num_threads = turbo_cpu_count();
    }

    coro_thread_pool_t *pool = (coro_thread_pool_t *)calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->thread_count = num_threads;
    atomic_store_explicit(&pool->next_thread, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->stopping, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->inflight_spawns, 0, memory_order_relaxed);
    turbo_mutex_init(&pool->lifecycle_mutex);
    turbo_cond_init(&pool->lifecycle_cond);

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
        if (wctx == NULL) {
            coro_thread_pool_destroy(pool);
            return NULL;
        }
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

    atomic_store_explicit(&pool->stopping, 1, memory_order_release);

    turbo_mutex_lock(&pool->lifecycle_mutex);
    while (atomic_load_explicit(&pool->inflight_spawns, memory_order_acquire) != 0) {
        turbo_cond_wait(&pool->lifecycle_cond, &pool->lifecycle_mutex);
    }
    turbo_mutex_unlock(&pool->lifecycle_mutex);

    /* 1. Let each context drain naturally after existing coroutines finish.
     * Clearing the persistent ref is enough to let the loop exit once
     * in-flight transport cleanup callbacks have posted back into the context.
     * An eager stop_requested short-circuits that drain and leaves external
     * refs behind during teardown. */
    for (int i = 0; i < pool->thread_count; i++) {
        if (pool->contexts[i]) {
            coro_context_set_persistent(pool->contexts[i], 0);
            if (coro_post(pool->contexts[i], coro_thread_pool_request_stop_post,
                          pool->contexts[i], NULL) != 0) {
                coro_context_stop(pool->contexts[i]);
            }
            turbo_loop_wake(pool->contexts[i]->loop);
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
    turbo_cond_destroy(&pool->lifecycle_cond);
    turbo_mutex_destroy(&pool->lifecycle_mutex);

    free(pool);
}

static void spawn_proxy(void *arg1, void *arg2) {
    coro_fn fn = (coro_fn)arg1;
    void *arg = arg2;
    if (coro_context_spawn(coro_context_current(), fn, arg) != 0) {
        abort();
    }
}

int coro_thread_pool_spawn(coro_thread_pool_t *pool, coro_fn fn, void *arg) {
    int rc;
    int idx;
    coro_context_t *target_ctx;

    if (!pool || !fn) return -1;
    if (coro_thread_pool_enter_spawn(pool) != 0) {
        return -1;
    }

    /* Use unsigned modulo semantics even after long runtimes. */
    idx = (int)((unsigned int)atomic_fetch_add_explicit(&pool->next_thread, 1,
                                                        memory_order_relaxed) %
                (unsigned int)pool->thread_count);
    target_ctx = pool->contexts[idx];
    if (target_ctx == NULL) {
        coro_thread_pool_leave_spawn(pool);
        return -1;
    }

    /* Good taste: Passes fn and arg directly through lock-free MPSC queue, zero allocations! */
    rc = coro_post(target_ctx, spawn_proxy, (void*)fn, arg);
    coro_thread_pool_leave_spawn(pool);
    return rc;
}

coro_context_t *coro_thread_pool_get_context(coro_thread_pool_t *pool, int index) {
    if (!pool || index < 0 || index >= pool->thread_count) return NULL;
    return pool->contexts[index];
}
