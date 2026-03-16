/**
 * test_bench_pools.c - Performance benchmarks for CoroNet pools.
 */

#include "CoroNet.h"
#include "tinytest.h"
#include "turbo_thread.h"
#include "turbo_atomic.h"
#include "turbo_coro.h"
#include "turbo_coro_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

#define BENCH_URL "tcp://127.0.0.1:18951"
/* Reduced to 10k to fit in memory with ASan overhead */
#define TOTAL_TASKS 10000

/* ── Helpers ───────────────────────────────────────────────── */

typedef struct {
    t_atomic_int_t count;
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
    t_atomic_inc((t_atomic_int_t*)arg);
}

static void nop_coro_sync(coro_t *co, void *arg) {
    (void)co;
    sync_counter_t *sc = (sync_counter_t*)arg;
    
    int new_count = t_atomic_inc(&sc->count);
    
    /* Only signal when all expected tasks complete to avoid lost wakeup */
    if (new_count == sc->expected) {
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
    t_atomic_bool_t *done;
} conn_bench_state_t;

static void conn_bench_task(coro_t *co, void *arg) {
    (void)co;
    conn_bench_state_t *s = (conn_bench_state_t *)arg;
    for (int i = 0; i < 1000; i++) {
        coro_socket_t *sock = NULL;
        if (coro_pool_borrow(s->p, &sock) == 0) coro_pool_return(s->p, sock);
    }
    t_atomic_store_bool(s->done, true);
}

static void open_pool_task(coro_t *co, void *arg) {
    (void)co;
    coro_pool_open((coro_pool_t*)arg, BENCH_URL);
}

/* ── Benchmarks ─────────────────────────────────────────────── */

spec("coro_pools_bench") {

    bench("coro_thread_pool") {
        coro_thread_pool_t *pool = coro_thread_pool_create(4);
        
        benchmark("spawn_throughput_10k", 20) {
            sync_counter_t sc;
            t_atomic_store(&sc.count, 0);
            sc.expected = TOTAL_TASKS;
            turbo_mutex_init(&sc.mutex);
            turbo_cond_init(&sc.cond);
            
            /* Spawn all tasks with backpressure handling */
            for (int i = 0; i < TOTAL_TASKS; i++) {
                int ret;
                int retry = 0;
                while ((ret = coro_thread_pool_spawn(pool, nop_coro_sync, &sc)) != 0) {
                    if (ret == TURBO_ENOMEM) {
                        /* Ring buffer full - yield and retry */
                        retry++;
                        if (retry > 1000) {
                            fprintf(stderr, "Failed to spawn task %d after 1000 retries\n", i);
                            sc.expected = i; /* Adjust expected count */
                            break;
                        }
                        #ifdef _WIN32
                            SwitchToThread();
                        #else
                            sched_yield();
                        #endif
                    } else {
                        fprintf(stderr, "Failed to spawn task %d: %d\n", i, ret);
                        sc.expected = i; /* Adjust expected count */
                        break;
                    }
                }
            }
            
            /* Wait for completion using condition variable (no busy-wait) */
            turbo_mutex_lock(&sc.mutex);
            while (t_atomic_load(&sc.count) < sc.expected) {
                turbo_cond_wait(&sc.cond, &sc.mutex);
            }
            turbo_mutex_unlock(&sc.mutex);
            
            turbo_cond_destroy(&sc.cond);
            turbo_mutex_destroy(&sc.mutex);
        }
        
        coro_thread_pool_destroy(pool);
    }

    bench("coro_object_pool") {
        coro_context_t *ctx = coro_context_create(NULL);
        coro_object_pool_config_t cfg = CORO_OBJECT_POOL_CONFIG_DEFAULT;
        cfg.initial_capacity = 1024; /* Be realistic with ASan */
        coro_object_pool_t *pool = coro_object_pool_create(&cfg, ctx);
        coro_t *cos[100];
        
        benchmark("acquire_release_10k", 100) {
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
        coro_context_t *ctx = coro_context_create(NULL);
        coro_socket_t *server = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
        /* Use a flag to ensure the server is ready before starting the pool */
        coro_socket_listen_url(server, BENCH_URL, bench_server_handler, NULL);
        
        coro_pool_config_t pool_cfg = CORO_POOL_CONFIG_DEFAULT;
        pool_cfg.min_size = 16;
        pool_cfg.max_size = 16;
        coro_pool_t *pool = coro_pool_create(ctx, &pool_cfg);
        
        coro_task_t *open_task = coro_task_create(ctx, open_pool_task, pool);
        coro_task_start(open_task);
        while(!coro_task_is_done(open_task)) coro_context_run(ctx, TURBO_RUN_ONCE);

        benchmark("borrow_return_1k", 50) {
            t_atomic_bool_t done;
            t_atomic_store_bool(&done, false);
            conn_bench_state_t s = { pool, &done };
            coro_context_spawn(ctx, conn_bench_task, &s);

            while (!t_atomic_load_bool(&done)) {
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
