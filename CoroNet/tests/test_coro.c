/**
 * @file test_coro.c
 * @brief Coroutine unit tests
 */

#include "turbo_coro.h"
#include "CoroNet/turbo_coro_pool.h"
#include "CoroNet/turbo_coro_context.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>

static int g_counter = 0;

// Simple coroutine that increments counter and yields
static void counter_coro(coro_t *co, void *arg) {
    UNUSED(co);
    int times = *(int *)arg;
    for (int i = 0; i < times; i++) {
        g_counter++;
        coro_yield();
    }
}

// Coroutine that uses push/pop for data passing
static void echo_coro(coro_t *co, void *arg) {
    UNUSED(arg);
    int value;
    while (coro_bytes_stored(co) >= sizeof(int)) {
        coro_pop(co, &value, sizeof(int));
        value *= 2;  // double it
        coro_push(co, &value, sizeof(int));
        coro_yield();
    }
}

// Producer coroutine
static void producer_coro(coro_t *co, void *arg) {
    UNUSED(co);
    int *results = (int *)arg;
    for (int i = 1; i <= 5; i++) {
        results[i - 1] = i * 10;
        coro_yield();
    }
}

// Nested yield test
static void nested_coro(coro_t *co, void *arg) {
    UNUSED(co);
    int *step = (int *)arg;
    *step = 1;
    coro_yield();
    *step = 2;
    coro_yield();
    *step = 3;
}

// Scheduler test coroutines
static void sched_worker(coro_t *co, void *arg) {
    UNUSED(co);
    int *counter = (int *)arg;
    for (int i = 0; i < 3; i++) {
        (*counter)++;
        coro_yield();
    }
}

static void sched_fast(coro_t *co, void *arg) {
    UNUSED(co);
    int *counter = (int *)arg;
    (*counter) += 10;
    // No yield - completes immediately
}

spec("Coroutine Tests") {
    before_each() {
        g_counter = 0;
    }

    it("should create and destroy coroutine") {
        int times = 3;
        coro_t *co = coro_create(counter_coro, &times, NULL);
        check(co != NULL);
        check_int_eq(coro_state(co), coro_SUSPENDED);
        coro_destroy(co);
    }

    it("should resume and yield") {
        int times = 3;
        coro_t *co = coro_create(counter_coro, &times, NULL);

        // Resume 3 times
        for (int i = 0; i < 3; i++) {
            check_int_eq(coro_alive(co), 1);
            coro_resume(co);
            check_int_eq(g_counter, i + 1);
        }

        // One more resume to finish
        coro_resume(co);
        check_int_eq(coro_alive(co), 0);
        check_int_eq(coro_state(co), coro_DEAD);

        coro_destroy(co);
    }

    it("should pass data via push/pop") {
        coro_t *co = coro_create(echo_coro, NULL, NULL);

        int input = 21;
        int output = 0;

        coro_push(co, &input, sizeof(int));
        coro_resume(co);
        coro_pop(co, &output, sizeof(int));

        check_int_eq(output, 42);  // 21 * 2

        coro_destroy(co);
    }

    it("should handle multiple yields") {
        int step = 0;
        coro_t *co = coro_create(nested_coro, &step, NULL);

        coro_resume(co);
        check_int_eq(step, 1);

        coro_resume(co);
        check_int_eq(step, 2);

        coro_resume(co);
        check_int_eq(step, 3);

        check_int_eq(coro_alive(co), 0);
        coro_destroy(co);
    }

    it("should support user data") {
        coro_opts_t opts = coro_OPTS_DEFAULT;
        int data = 123;
        opts.user_data = &data;

        int times = 1;
        coro_t *co = coro_create(counter_coro, &times, &opts);

        check(coro_get_data(co) == &data);

        int new_data = 456;
        coro_set_data(co, &new_data);
        check(coro_get_data(co) == &new_data);

        coro_destroy(co);
    }

    it("should get running coroutine") {
        // Outside coroutine, should be NULL
        check(coro_running() == NULL);
    }

    it("should produce values") {
        int results[5] = {0};
        coro_t *co = coro_create(producer_coro, results, NULL);

        for (int i = 0; i < 5; i++) {
            coro_resume(co);
            check_int_eq(results[i], (i + 1) * 10);
        }

        coro_destroy(co);
    }
}

spec("Object Pool Tests") {
    it("should create and destroy pool") {
        coro_context_t *ctx = coro_context_create(NULL);
        check_not_null(ctx);
        
        coro_object_pool_config_t config = CORO_OBJECT_POOL_CONFIG_DEFAULT;
        coro_object_pool_t *pool = coro_object_pool_create(&config, ctx);
        check_not_null(pool);
        check_int_eq(coro_object_pool_free_count(pool), 16);
        check_int_eq(coro_object_pool_active_count(pool), 0);
        coro_object_pool_destroy(pool);
        
        coro_context_destroy(ctx);
    }

    it("should acquire and release coroutines") {
        coro_context_t *ctx = coro_context_create(NULL);
        check_not_null(ctx);
        
        coro_object_pool_config_t config = {.initial_capacity = 2, .max_capacity = 4, .stack_size = 0};
        coro_object_pool_t *pool = coro_object_pool_create(&config, ctx);
        
        int counter = 0;
        coro_t *co1 = coro_object_pool_acquire(pool, sched_worker, &counter);
        check_not_null(co1);
        check_int_eq(coro_object_pool_active_count(pool), 1);
        check_int_eq(coro_object_pool_free_count(pool), 1);
        
        coro_t *co2 = coro_object_pool_acquire(pool, sched_worker, &counter);
        check_not_null(co2);
        check_int_eq(coro_object_pool_active_count(pool), 2);
        check_int_eq(coro_object_pool_free_count(pool), 0);
        
        /* Run to completion */
        while (coro_alive(co1)) coro_resume(co1);
        while (coro_alive(co2)) coro_resume(co2);
        
        coro_object_pool_release(pool, co1);
        coro_object_pool_release(pool, co2);
        
        check_int_eq(coro_object_pool_active_count(pool), 0);
        check_int_eq(coro_object_pool_free_count(pool), 2);
        
        coro_object_pool_destroy(pool);
        coro_context_destroy(ctx);
    }

    it("should respect max capacity") {
        coro_context_t *ctx = coro_context_create(NULL);
        check_not_null(ctx);
        
        coro_object_pool_config_t config = {.initial_capacity = 1, .max_capacity = 2, .stack_size = 0};
        coro_object_pool_t *pool = coro_object_pool_create(&config, ctx);
        
        int counter = 0;
        coro_t *co1 = coro_object_pool_acquire(pool, sched_fast, &counter);
        coro_t *co2 = coro_object_pool_acquire(pool, sched_fast, &counter);
        coro_t *co3 = coro_object_pool_acquire(pool, sched_fast, &counter);
        
        check_not_null(co1);
        check_not_null(co2);
        check(co3 == NULL);  /* Exceeds max capacity */
        
        /* Run to completion and release before destroy so assert passes */
        while (coro_alive(co1)) coro_resume(co1);
        coro_object_pool_release(pool, co1);
        while (coro_alive(co2)) coro_resume(co2);
        coro_object_pool_release(pool, co2);
        
        coro_object_pool_destroy(pool);
        coro_context_destroy(ctx);
    }

    it("should reuse coroutines") {
        coro_context_t *ctx = coro_context_create(NULL);
        check_not_null(ctx);
        
        coro_object_pool_config_t config = {.initial_capacity = 1, .max_capacity = 1, .stack_size = 0};
        coro_object_pool_t *pool = coro_object_pool_create(&config, ctx);
        
        int c1 = 0, c2 = 0;
        
        coro_t *co1 = coro_object_pool_acquire(pool, sched_worker, &c1);
        check_not_null(co1);
        while (coro_alive(co1)) coro_resume(co1);
        coro_object_pool_release(pool, co1);
        
        coro_t *co2 = coro_object_pool_acquire(pool, sched_worker, &c2);
        check_not_null(co2);
        check(co1 == co2);  /* Same coroutine reused */
        while (coro_alive(co2)) coro_resume(co2);
        coro_object_pool_release(pool, co2);
        
        check_int_eq(c1, 3);
        check_int_eq(c2, 3);
        
        coro_object_pool_destroy(pool);
        coro_context_destroy(ctx);
    }
}

spec("Scheduler Tests") {
    it("should create and destroy scheduler") {
        coro_scheduler_t *sched = coro_scheduler_create();
        check(sched != NULL);
        check_int_eq(coro_scheduler_count(sched), 0);
        coro_scheduler_destroy(sched);
    }

    it("should spawn coroutines") {
        coro_scheduler_t *sched = coro_scheduler_create();
        int counter = 0;

        coro_t *co = coro_spawn(sched, sched_worker, &counter, NULL);
        check(co != NULL);
        check_int_eq(coro_scheduler_count(sched), 1);

        // Run to completion before destroying
        coro_scheduler_run(sched);
        check_int_eq(coro_scheduler_count(sched), 0);

        coro_scheduler_destroy(sched);
    }

    it("should run single coroutine to completion") {
        coro_scheduler_t *sched = coro_scheduler_create();
        int counter = 0;

        coro_spawn(sched, sched_worker, &counter, NULL);
        coro_scheduler_run(sched);

        check_int_eq(counter, 3);  // 3 iterations
        check_int_eq(coro_scheduler_count(sched), 0);

        coro_scheduler_destroy(sched);
    }

    it("should run multiple coroutines concurrently") {
        coro_scheduler_t *sched = coro_scheduler_create();
        int c1 = 0, c2 = 0, c3 = 0;

        coro_spawn(sched, sched_worker, &c1, NULL);
        coro_spawn(sched, sched_worker, &c2, NULL);
        coro_spawn(sched, sched_worker, &c3, NULL);

        check_int_eq(coro_scheduler_count(sched), 3);

        coro_scheduler_run(sched);

        check_int_eq(c1, 3);
        check_int_eq(c2, 3);
        check_int_eq(c3, 3);
        check_int_eq(coro_scheduler_count(sched), 0);

        coro_scheduler_destroy(sched);
    }

    it("should handle fast-completing coroutines") {
        coro_scheduler_t *sched = coro_scheduler_create();
        int counter = 0;

        coro_spawn(sched, sched_fast, &counter, NULL);
        coro_spawn(sched, sched_fast, &counter, NULL);

        coro_scheduler_run(sched);

        check_int_eq(counter, 20);  // 10 + 10
        check_int_eq(coro_scheduler_count(sched), 0);

        coro_scheduler_destroy(sched);
    }

    it("should tick one round at a time") {
        coro_scheduler_t *sched = coro_scheduler_create();
        int c1 = 0, c2 = 0;

        coro_spawn(sched, sched_worker, &c1, NULL);
        coro_spawn(sched, sched_worker, &c2, NULL);

        // First tick
        int alive = coro_scheduler_tick(sched);
        check_int_eq(alive, 2);
        check_int_eq(c1, 1);
        check_int_eq(c2, 1);

        // Second tick
        alive = coro_scheduler_tick(sched);
        check_int_eq(alive, 2);
        check_int_eq(c1, 2);
        check_int_eq(c2, 2);

        // Third tick
        alive = coro_scheduler_tick(sched);
        check_int_eq(alive, 2);
        check_int_eq(c1, 3);
        check_int_eq(c2, 3);

        // Fourth tick - coroutines complete
        alive = coro_scheduler_tick(sched);
        check_int_eq(alive, 0);

        coro_scheduler_destroy(sched);
    }

    it("should provide current scheduler") {
        // Outside scheduler, should be NULL
        check(coro_current_scheduler() == NULL);
    }
}
