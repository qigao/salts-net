/**
 * @file test_coro.c
 * @brief Coroutine unit tests
 */

#include "turbo_coro.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>

static int g_counter = 0;

// Simple coroutine that increments counter and yields
static void counter_coro(turbo_coro_t *co, void *arg) {
    UNUSED(co);
    int times = *(int *)arg;
    for (int i = 0; i < times; i++) {
        g_counter++;
        turbo_coro_yield();
    }
}

// Coroutine that uses push/pop for data passing
static void echo_coro(turbo_coro_t *co, void *arg) {
    UNUSED(arg);
    int value;
    while (turbo_coro_bytes_stored(co) >= sizeof(int)) {
        turbo_coro_pop(co, &value, sizeof(int));
        value *= 2;  // double it
        turbo_coro_push(co, &value, sizeof(int));
        turbo_coro_yield();
    }
}

// Producer coroutine
static void producer_coro(turbo_coro_t *co, void *arg) {
    UNUSED(co);
    int *results = (int *)arg;
    for (int i = 1; i <= 5; i++) {
        results[i - 1] = i * 10;
        turbo_coro_yield();
    }
}

// Nested yield test
static void nested_coro(turbo_coro_t *co, void *arg) {
    UNUSED(co);
    int *step = (int *)arg;
    *step = 1;
    turbo_coro_yield();
    *step = 2;
    turbo_coro_yield();
    *step = 3;
}

// Scheduler test coroutines
static void sched_worker(turbo_coro_t *co, void *arg) {
    UNUSED(co);
    int *counter = (int *)arg;
    for (int i = 0; i < 3; i++) {
        (*counter)++;
        turbo_coro_yield();
    }
}

static void sched_fast(turbo_coro_t *co, void *arg) {
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
        turbo_coro_t *co = turbo_coro_create(counter_coro, &times, NULL);
        check(co != NULL);
        check_int_eq(turbo_coro_state(co), TURBO_CORO_SUSPENDED);
        turbo_coro_destroy(co);
    }

    it("should resume and yield") {
        int times = 3;
        turbo_coro_t *co = turbo_coro_create(counter_coro, &times, NULL);

        // Resume 3 times
        for (int i = 0; i < 3; i++) {
            check_int_eq(turbo_coro_alive(co), 1);
            turbo_coro_resume(co);
            check_int_eq(g_counter, i + 1);
        }

        // One more resume to finish
        turbo_coro_resume(co);
        check_int_eq(turbo_coro_alive(co), 0);
        check_int_eq(turbo_coro_state(co), TURBO_CORO_DEAD);

        turbo_coro_destroy(co);
    }

    it("should pass data via push/pop") {
        turbo_coro_t *co = turbo_coro_create(echo_coro, NULL, NULL);

        int input = 21;
        int output = 0;

        turbo_coro_push(co, &input, sizeof(int));
        turbo_coro_resume(co);
        turbo_coro_pop(co, &output, sizeof(int));

        check_int_eq(output, 42);  // 21 * 2

        turbo_coro_destroy(co);
    }

    it("should handle multiple yields") {
        int step = 0;
        turbo_coro_t *co = turbo_coro_create(nested_coro, &step, NULL);

        turbo_coro_resume(co);
        check_int_eq(step, 1);

        turbo_coro_resume(co);
        check_int_eq(step, 2);

        turbo_coro_resume(co);
        check_int_eq(step, 3);

        check_int_eq(turbo_coro_alive(co), 0);
        turbo_coro_destroy(co);
    }

    it("should support user data") {
        turbo_coro_opts_t opts = TURBO_CORO_OPTS_DEFAULT;
        int data = 123;
        opts.user_data = &data;

        int times = 1;
        turbo_coro_t *co = turbo_coro_create(counter_coro, &times, &opts);

        check(turbo_coro_get_data(co) == &data);

        int new_data = 456;
        turbo_coro_set_data(co, &new_data);
        check(turbo_coro_get_data(co) == &new_data);

        turbo_coro_destroy(co);
    }

    it("should get running coroutine") {
        // Outside coroutine, should be NULL
        check(turbo_coro_running() == NULL);
    }

    it("should produce values") {
        int results[5] = {0};
        turbo_coro_t *co = turbo_coro_create(producer_coro, results, NULL);

        for (int i = 0; i < 5; i++) {
            turbo_coro_resume(co);
            check_int_eq(results[i], (i + 1) * 10);
        }

        turbo_coro_destroy(co);
    }
}

spec("Scheduler Tests") {
    it("should create and destroy scheduler") {
        turbo_coro_scheduler_t *sched = turbo_coro_scheduler_create();
        check(sched != NULL);
        check_int_eq(turbo_coro_scheduler_count(sched), 0);
        turbo_coro_scheduler_destroy(sched);
    }

    it("should spawn coroutines") {
        turbo_coro_scheduler_t *sched = turbo_coro_scheduler_create();
        int counter = 0;

        turbo_coro_t *co = turbo_coro_spawn(sched, sched_worker, &counter);
        check(co != NULL);
        check_int_eq(turbo_coro_scheduler_count(sched), 1);

        turbo_coro_scheduler_destroy(sched);
    }

    it("should run single coroutine to completion") {
        turbo_coro_scheduler_t *sched = turbo_coro_scheduler_create();
        int counter = 0;

        turbo_coro_spawn(sched, sched_worker, &counter);
        turbo_coro_scheduler_run(sched);

        check_int_eq(counter, 3);  // 3 iterations
        check_int_eq(turbo_coro_scheduler_count(sched), 0);

        turbo_coro_scheduler_destroy(sched);
    }

    it("should run multiple coroutines concurrently") {
        turbo_coro_scheduler_t *sched = turbo_coro_scheduler_create();
        int c1 = 0, c2 = 0, c3 = 0;

        turbo_coro_spawn(sched, sched_worker, &c1);
        turbo_coro_spawn(sched, sched_worker, &c2);
        turbo_coro_spawn(sched, sched_worker, &c3);

        check_int_eq(turbo_coro_scheduler_count(sched), 3);

        turbo_coro_scheduler_run(sched);

        check_int_eq(c1, 3);
        check_int_eq(c2, 3);
        check_int_eq(c3, 3);
        check_int_eq(turbo_coro_scheduler_count(sched), 0);

        turbo_coro_scheduler_destroy(sched);
    }

    it("should handle fast-completing coroutines") {
        turbo_coro_scheduler_t *sched = turbo_coro_scheduler_create();
        int counter = 0;

        turbo_coro_spawn(sched, sched_fast, &counter);
        turbo_coro_spawn(sched, sched_fast, &counter);

        turbo_coro_scheduler_run(sched);

        check_int_eq(counter, 20);  // 10 + 10
        check_int_eq(turbo_coro_scheduler_count(sched), 0);

        turbo_coro_scheduler_destroy(sched);
    }

    it("should tick one round at a time") {
        turbo_coro_scheduler_t *sched = turbo_coro_scheduler_create();
        int c1 = 0, c2 = 0;

        turbo_coro_spawn(sched, sched_worker, &c1);
        turbo_coro_spawn(sched, sched_worker, &c2);

        // First tick
        int alive = turbo_coro_scheduler_tick(sched);
        check_int_eq(alive, 2);
        check_int_eq(c1, 1);
        check_int_eq(c2, 1);

        // Second tick
        alive = turbo_coro_scheduler_tick(sched);
        check_int_eq(alive, 2);
        check_int_eq(c1, 2);
        check_int_eq(c2, 2);

        // Third tick
        alive = turbo_coro_scheduler_tick(sched);
        check_int_eq(alive, 2);
        check_int_eq(c1, 3);
        check_int_eq(c2, 3);

        // Fourth tick - coroutines complete
        alive = turbo_coro_scheduler_tick(sched);
        check_int_eq(alive, 0);

        turbo_coro_scheduler_destroy(sched);
    }

    it("should provide current scheduler") {
        // Outside scheduler, should be NULL
        check(turbo_coro_current_scheduler() == NULL);
    }
}
