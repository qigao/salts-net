/**
 * test_bench_pools.c - Performance benchmarks for CoroNet pools.
 */

#include "CoroNet.h"
#include "tinytest.h"
#include "turbo_thread.h"
#include <stdatomic.h>
#include "turbo_coro.h"
#include "CoroNet/turbo_coro_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

#define BENCH_HOST "127.0.0.1"
#define BENCH_PORT 18951
/* Reduced to 10k to fit in memory with ASan overhead */
#define TOTAL_TASKS 10000

/* ── Helpers ───────────────────────────────────────────────── */

typedef struct {
    atomic_int count;
    int expected;
    turbo_mutex_t mutex;
    turbo_cond_t cond;
} sync_counter_t;

static void nop_coro(coro_t *co, void *arg) {
    (void)co;
    (void)arg;
    /* Do nothing - for object pool benchmark */
}

static void nop_coro_count(coro_t *co, void *arg) {
    (void)co;
    atomic_fetch_add_explicit((atomic_int*)arg, 1, memory_order_relaxed);
}

static void nop_coro_sync(coro_t *co, void *arg) {
    (void)co;
    sync_counter_t *sc = (sync_counter_t*)arg;

    /* Relaxed increment - we only care about the final value */
    int old = atomic_fetch_add_explicit(&sc->count, 1, memory_order_relaxed);

    /* Only the last task signals (no mutex needed for signal-only) */
    if (old + 1 == sc->expected) {
        atomic_thread_fence(memory_order_acquire);
        turbo_mutex_lock(&sc->mutex);
        turbo_cond_signal(&sc->cond);
        turbo_mutex_unlock(&sc->mutex);
    }
}

static void bench_server_handler(coro_socket_t *client, void *arg) {
    (void)arg;
    /* Just wait for the client to close (EOF) to clean up cleanly */
    char *buf;
    size_t len;
    while (coro_socket_recv(client, &buf, &len) == 0) {}
    coro_socket_destroy(client);
}

typedef struct {
    coro_pool_t *p;
    _Atomic int *done;
} conn_bench_state_t;

static void conn_bench_task(coro_t *co, void *arg) {
    (void)co;
    conn_bench_state_t *s = (conn_bench_state_t *)arg;
    for (int i = 0; i < 1000; i++) {
        coro_socket_t *sock = NULL;
        if (coro_pool_borrow(s->p, &sock) == 0) coro_pool_return(s->p, sock);
    }
    atomic_store_explicit(s->done, 1, memory_order_release);
}

static void open_pool_task(coro_t *co, void *arg) {
    (void)co;
    coro_pool_open((coro_pool_t*)arg, BENCH_HOST, BENCH_PORT, CORO_SOCKET_TCP_V4);
}

/* ── Benchmarks ─────────────────────────────────────────────── */

spec("coro_pools_bench") {

    bench("coro_thread_pool") {

        benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
        coro_thread_pool_t *pool = coro_thread_pool_create(4);
        
        benchmark("spawn_throughput_10k", 20, 1) {
            sync_counter_t sc;
            atomic_store(&sc.count, 0);
            sc.expected = TOTAL_TASKS;
            turbo_mutex_init(&sc.mutex);
            turbo_cond_init(&sc.cond);

            /* Direct spawn - ring buffer is 16384*4=65536 slots, more than enough for 10k tasks */
            for (int i = 0; i < TOTAL_TASKS; i++) {
                int ret = coro_thread_pool_spawn(pool, nop_coro_sync, &sc);
                if (ret != 0) {
                    /* This should never happen - if it does, it's a design flaw */
                    fprintf(stderr, "ERROR: Spawn failed at task %d with error %d\n", i, ret);
                    sc.expected = i;
                    break;
                }
            }

            /* Wait for completion */
            turbo_mutex_lock(&sc.mutex);
            while (atomic_load_explicit(&sc.count, memory_order_acquire) < sc.expected) {
                turbo_cond_wait(&sc.cond, &sc.mutex);
            }
            turbo_mutex_unlock(&sc.mutex);

            turbo_cond_destroy(&sc.cond);
            turbo_mutex_destroy(&sc.mutex);
        }
        
        coro_thread_pool_destroy(pool);
    }

    bench("coro_object_pool") {

        benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
        coro_context_t *ctx = coro_context_create(NULL);
        coro_object_pool_config_t cfg = CORO_OBJECT_POOL_CONFIG_DEFAULT;
        cfg.initial_capacity = 1024; /* Be realistic with ASan */
        coro_object_pool_t *pool = coro_object_pool_create(&cfg, ctx);
        coro_t *cos[100];
        
        benchmark("acquire_release_10k", 100, 1) {
            for (int i = 0; i < 100; i++) {
                for(int j=0; j<100; j++) cos[j] = coro_object_pool_acquire(pool, nop_coro, NULL);
                for(int j=0; j<100; j++) {
                    while(coro_alive(cos[j])) coro_resume(cos[j]);
                    coro_object_pool_release(pool, cos[j]);
                }
            }
        }
        
        coro_object_pool_destroy(pool);
        coro_context_destroy(ctx);
    }

    bench("coro_connection_pool") {

        benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *server = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
        /* Use a flag to ensure the server is ready before starting the pool */
        coro_socket_listen_on(server, BENCH_HOST, BENCH_PORT, bench_server_handler, NULL);
        
        coro_pool_config_t pool_cfg = CORO_POOL_CONFIG_DEFAULT;
        pool_cfg.min_size = 16;
        pool_cfg.max_size = 16;
        coro_pool_t *pool = coro_pool_create(ctx, &pool_cfg);
        
        coro_task_t *open_task = coro_task_create(ctx, open_pool_task, pool);
        coro_task_start(open_task);
        while(!coro_task_is_done(open_task)) coro_context_run(ctx, TURBO_RUN_ONCE);

        benchmark("borrow_return_1k", 50, 1) {
            _Atomic int done = 0;
            conn_bench_state_t s = { pool, &done };
            coro_context_spawn(ctx, conn_bench_task, &s);

            while (!atomic_load_explicit(&done, memory_order_acquire)) {
                coro_context_run(ctx, TURBO_RUN_ONCE);
            }
        }

        coro_pool_destroy(pool);
        coro_socket_destroy(server);

        /* Pump the loop to let server handler coroutines process the closed connections and exit natively */
        for (int i = 0; i < 50; i++) {
            coro_context_run(ctx, TURBO_RUN_NOWAIT);
        }

        coro_context_destroy(ctx);
    }
}
