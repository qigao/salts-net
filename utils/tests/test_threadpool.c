/**
 * @file test_threadpool.c
 * @brief Thread pool unit tests
 */

#include "turbo_thread.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>

#define UNUSED(x) (void)(x)
#define NUM_TASKS 100

static turbo_mutex_t counter_mutex;
static volatile int counter = 0;
static atomic_int gate_open;
static atomic_int mpmc_counter;

typedef struct {
    turbo_threadpool_t *pool;
    int tasks;
    atomic_int *submit_failures;
} submitter_ctx_t;

static void increment_task(void *arg) {
    UNUSED(arg);
    turbo_mutex_lock(&counter_mutex);
    counter++;
    turbo_mutex_unlock(&counter_mutex);
}

static void slow_task(void *arg) {
    int *result = (int *)arg;
    turbo_sleep_ms(10);
    *result = 42;
}

static void sum_task(void *arg) {
    int *val = (int *)arg;
    turbo_mutex_lock(&counter_mutex);
    counter += *val;
    turbo_mutex_unlock(&counter_mutex);
}

static void gated_task(void *arg) {
    UNUSED(arg);
    while (!atomic_load(&gate_open)) {
        turbo_sleep_ms(1);
    }
}

static void mpmc_count_task(void *arg) {
    UNUSED(arg);
    atomic_fetch_add(&mpmc_counter, 1);
}

static void submitter_thread(void *arg) {
    submitter_ctx_t *ctx = (submitter_ctx_t *)arg;
    for (int i = 0; i < ctx->tasks; ++i) {
        if (turbo_threadpool_submit(ctx->pool, mpmc_count_task, NULL) != 0) {
            atomic_fetch_add(ctx->submit_failures, 1);
        }
    }
}

spec("Thread Pool Tests") {
    before_each() {
        turbo_mutex_init(&counter_mutex);
        counter = 0;
        atomic_store(&gate_open, 0);
        atomic_store(&mpmc_counter, 0);
    }

    after_each() {
        turbo_mutex_destroy(&counter_mutex);
    }

    it("should create and destroy pool") {
        turbo_threadpool_t *pool = turbo_threadpool_create(2);
        check(pool != NULL);
        check_int_eq(turbo_threadpool_size(pool), 2);
        turbo_threadpool_destroy(pool);
    }

    it("should auto-detect CPU cores when 0") {
        turbo_threadpool_t *pool = turbo_threadpool_create(0);
        check(pool != NULL);
        check(turbo_threadpool_size(pool) >= 1);
        printf("  (detected %d cores)\n", turbo_threadpool_size(pool));
        turbo_threadpool_destroy(pool);
    }

    it("should execute single task") {
        turbo_threadpool_t *pool = turbo_threadpool_create(2);

        int result = 0;
        turbo_threadpool_submit(pool, slow_task, &result);
        turbo_threadpool_wait(pool);

        check_int_eq(result, 42);
        turbo_threadpool_destroy(pool);
    }

    it("should execute many tasks") {
        turbo_threadpool_t *pool = turbo_threadpool_create(4);

        for (int i = 0; i < NUM_TASKS; i++) {
            turbo_threadpool_submit(pool, increment_task, NULL);
        }

        turbo_threadpool_wait(pool);
        check_int_eq(counter, NUM_TASKS);

        turbo_threadpool_destroy(pool);
    }

    it("should accept tasks from multiple producers") {
        enum { PRODUCERS = 4, TASKS_PER_PRODUCER = 250 };
        turbo_threadpool_t *pool = turbo_threadpool_create(4);
        turbo_thread_t producers[PRODUCERS];
        submitter_ctx_t contexts[PRODUCERS];
        atomic_int submit_failures;

        atomic_store(&submit_failures, 0);
        check(pool != NULL);

        for (int i = 0; i < PRODUCERS; ++i) {
            contexts[i].pool = pool;
            contexts[i].tasks = TASKS_PER_PRODUCER;
            contexts[i].submit_failures = &submit_failures;
            check_int_eq(turbo_thread_create(&producers[i], submitter_thread, &contexts[i]), 0);
        }

        for (int i = 0; i < PRODUCERS; ++i) {
            turbo_thread_join(&producers[i]);
        }

        turbo_threadpool_wait(pool);
        check_int_eq(atomic_load(&submit_failures), 0);
        check_int_eq(atomic_load(&mpmc_counter), PRODUCERS * TASKS_PER_PRODUCER);

        turbo_threadpool_destroy(pool);
    }

    it("should handle more tasks than threads") {
        turbo_threadpool_t *pool = turbo_threadpool_create(2);

        for (int i = 0; i < 50; i++) {
            turbo_threadpool_submit(pool, increment_task, NULL);
        }

        turbo_threadpool_wait(pool);
        check_int_eq(counter, 50);

        turbo_threadpool_destroy(pool);
    }

    it("should pass arguments correctly") {
        turbo_threadpool_t *pool = turbo_threadpool_create(4);

        int values[10];
        for (int i = 0; i < 10; i++) {
            values[i] = i + 1;  // 1..10
            turbo_threadpool_submit(pool, sum_task, &values[i]);
        }

        turbo_threadpool_wait(pool);
        // Sum of 1..10 = 55
        check_int_eq(counter, 55);

        turbo_threadpool_destroy(pool);
    }

    it("should report pending count") {
        turbo_threadpool_t *pool = turbo_threadpool_create(1);

        // Submit slow tasks
        int results[5] = {0};
        for (int i = 0; i < 5; i++) {
            turbo_threadpool_submit(pool, slow_task, &results[i]);
        }

        // Should have pending tasks
        check(turbo_threadpool_pending(pool) > 0);

        turbo_threadpool_wait(pool);
        check_int_eq(turbo_threadpool_pending(pool), 0);

        turbo_threadpool_destroy(pool);
    }

    it("should handle empty wait") {
        turbo_threadpool_t *pool = turbo_threadpool_create(2);

        // Wait with no tasks should return immediately
        turbo_threadpool_wait(pool);
        check_int_eq(turbo_threadpool_pending(pool), 0);

        turbo_threadpool_destroy(pool);
    }

    it("should reject tasks after shutdown") {
        turbo_threadpool_t *pool = turbo_threadpool_create(2);
        turbo_threadpool_shutdown(pool);
        check_int_eq(turbo_threadpool_is_accepting(pool), 0);
        check_int_eq(turbo_threadpool_submit(pool, increment_task, NULL), -1);
        turbo_threadpool_destroy(pool);

        check(1);
    }

    it("should honor configured queue capacity for try_submit") {
        turbo_threadpool_config_t config = {
            .num_threads = 1,
            .queue_capacity = 2,
        };
        turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&config);
        turbo_threadpool_stats_t stats = {0};
        int accepted = 0;
        int rejected = 0;
        int attempts = 0;

        check(pool != NULL);
        check_int_eq((int)turbo_threadpool_capacity(pool), 2);

        while (accepted < 1 && attempts < 200) {
            if (turbo_threadpool_try_submit(pool, gated_task, NULL) == 0) {
                accepted++;
                break;
            }
            turbo_sleep_ms(1);
            attempts++;
        }
        check_int_eq(accepted, 1);

        attempts = 0;
        do {
            turbo_threadpool_get_stats(pool, &stats);
            if (stats.active_tasks >= 1) {
                break;
            }
            turbo_sleep_ms(1);
            attempts++;
        } while (attempts < 200);
        check_int_eq((int)stats.active_tasks, 1);

        attempts = 0;
        while ((accepted < 3 || rejected < 1) && attempts < 400) {
            if (turbo_threadpool_try_submit(pool, gated_task, NULL) == 0) {
                accepted++;
            } else {
                rejected++;
            }
            turbo_sleep_ms(1);
            attempts++;
        }

        turbo_threadpool_get_stats(pool, &stats);
        check_int_eq(accepted, 3);
        check(rejected >= 1);
        check_int_eq((int)stats.rejected_tasks, 1);
        check_int_eq((int)stats.active_tasks, 1);
        check_int_eq((int)stats.queued_tasks, 2);
        check_int_eq((int)stats.pending_tasks, 3);

        atomic_store(&gate_open, 1);
        turbo_threadpool_wait(pool);
        turbo_threadpool_get_stats(pool, &stats);
        check_int_eq((int)stats.pending_tasks, 0);
        turbo_threadpool_destroy(pool);
    }
}
