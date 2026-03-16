#include "CoroNet/turbo_coro_thread_pool.h"
#include "turbo_coro_internal.h"
#include "turbo_thread.h"
#include "turbo_atomic.h"
#include <stdlib.h>
#include <uv.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

#define UNUSED(x) (void)(x)

/* ── Private task proxy structure ─────────────────────────── */

typedef struct {
    coro_context_t *ctx;
    coro_fn fn;
    void *arg;
    mem_buffer_t *buffer; /**< Underlying buffer for recycling */
} proxy_arg_t;

struct coro_thread_pool_s {
    coro_context_t **contexts;
    turbo_thread_t *threads;
    int thread_count;
    t_atomic_int_t next_thread;
    volatile int stopping;
    mem_pool_t* task_arena;        /**< Arena for task proxies */
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
    t_atomic_store(&pool->next_thread, 0);
    
    /* Initialize task proxy arena with pre-allocated size */
    pool->task_arena = (mem_pool_t*)calloc(1, sizeof(mem_pool_t));
    if (!pool->task_arena || mem_init(pool->task_arena, MEM_ARENA_POOL_INIT_SIZE) != 0) {
        if (pool->task_arena) free(pool->task_arena);
        coro_thread_pool_destroy(pool);
        return NULL;
    }

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

static void spawn_proxy(void *arg) {
    proxy_arg_t *pa = (proxy_arg_t *)arg;
    coro_context_spawn(pa->ctx, pa->fn, pa->arg);
    
    /* Good taste: Recycle the proxy back to the pool-wide arena */
    if (pa->buffer) {
        mem_release(pa->buffer);
    }
}

int coro_thread_pool_spawn(coro_thread_pool_t *pool, coro_fn fn, void *arg) {
    if (!pool || !fn || pool->stopping) return -1;

    /* Use atomic increment for fair distribution */
    int idx = t_atomic_fetch_add(&pool->next_thread, 1) % pool->thread_count;
    coro_context_t *target_ctx = pool->contexts[idx];

    /* Good taste: Use recycled buffers from the task arena instead of malloc */
    mem_buffer_t *buf = mem_get_buffer(pool->task_arena, sizeof(proxy_arg_t));
    if (!buf) return -2;

    proxy_arg_t *pa = (proxy_arg_t *)buf->data;
    pa->ctx = target_ctx;
    pa->fn = fn;
    pa->arg = arg;
    pa->buffer = buf;

    int ret = coro_post(target_ctx, spawn_proxy, pa);
    if (ret != 0) {
        mem_release(buf);
    }
    return ret;
}

coro_context_t *coro_thread_pool_get_context(coro_thread_pool_t *pool, int index) {
    if (!pool || index < 0 || index >= pool->thread_count) return NULL;
    return pool->contexts[index];
}
