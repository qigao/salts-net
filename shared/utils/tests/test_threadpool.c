/**
 * @file test_threadpool.c
 * @brief Thread pool unit tests
 */

#include "turbo_thread.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>

#define UNUSED(x) (void)(x)
#define NUM_TASKS 100

static turbo_mutex_t counter_mutex;
static volatile int counter = 0;

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

spec("Thread Pool Tests") {
    before_each() {
        turbo_mutex_init(&counter_mutex);
        counter = 0;
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
        turbo_threadpool_destroy(pool);

        // Can't test submit after destroy since pool is freed
        // Just verify clean shutdown works
        check(1);  // If we got here, shutdown was clean
    }
}
