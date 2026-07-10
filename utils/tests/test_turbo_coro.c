/**
 * @file test_turbo_coro.c
 * @brief Tests for the TurboNet coroutine primitive.
 */

#include "tinytest.h"
#include "turbo_coro.h"
#include "turbo_coro_pool.h"

static int g_counter = 0;
static int g_discard_count = 0;

static void counter_coro(coro_t *co, void *arg) {
    UNUSED(co);
    int times = *(int *)arg;
    for (int i = 0; i < times; i++) {
        g_counter++;
        coro_yield();
    }
}

static void echo_coro(coro_t *co, void *arg) {
    UNUSED(arg);
    int value = 0;
    while (coro_bytes_stored(co) >= sizeof(int)) {
        coro_pop(co, &value, sizeof(value));
        value *= 2;
        coro_push(co, &value, sizeof(value));
        coro_yield();
    }
}

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
}

static void discard_callback(coro_t *co, void *arg) {
    UNUSED(co);
    int *counter = (int *)arg;
    (*counter)++;
}

spec("Turbo Coro Primitive") {
    before_each() {
        g_counter = 0;
        g_discard_count = 0;
    }

    it("creates, resumes, yields, and destroys a coroutine") {
        int times = 3;
        coro_t *co = coro_create(counter_coro, &times, NULL);
        check_not_null(co);
        check_int_eq(coro_state(co), coro_SUSPENDED);

        for (int i = 0; i < times; i++) {
            check_int_eq(coro_alive(co), 1);
            check_int_eq(coro_resume(co), 0);
            check_int_eq(g_counter, i + 1);
        }

        check_int_eq(coro_resume(co), 0);
        check_int_eq(coro_alive(co), 0);
        coro_destroy(co);
    }

    it("passes data through coroutine storage") {
        coro_t *co = coro_create(echo_coro, NULL, NULL);
        int input = 21;
        int output = 0;

        check_not_null(co);
        check_int_eq(coro_push(co, &input, sizeof(input)), 0);
        check_int_eq(coro_resume(co), 0);
        check_int_eq(coro_pop(co, &output, sizeof(output)), 0);
        check_int_eq(output, 42);

        coro_destroy(co);
    }

    it("stores user data") {
        coro_opts_t opts = coro_OPTS_DEFAULT;
        int data = 123;
        int new_data = 456;
        int times = 1;
        opts.user_data = &data;

        coro_t *co = coro_create(counter_coro, &times, &opts);
        check_not_null(co);
        check(coro_get_data(co) == &data);

        coro_set_data(co, &new_data);
        check(coro_get_data(co) == &new_data);

        coro_destroy(co);
    }

    it("runs scheduled coroutines to completion") {
        coro_scheduler_t *sched = coro_scheduler_create();
        int c1 = 0;
        int c2 = 0;

        check_not_null(sched);
        check_not_null(coro_spawn(sched, sched_worker, &c1, NULL));
        check_not_null(coro_spawn(sched, sched_worker, &c2, NULL));
        check_int_eq(coro_scheduler_count(sched), 2);

        coro_scheduler_run(sched);
        check_int_eq(c1, 3);
        check_int_eq(c2, 3);
        check_int_eq(coro_scheduler_count(sched), 0);

        coro_scheduler_destroy(sched);
    }

    it("handles fast-completing scheduled coroutines") {
        coro_scheduler_t *sched = coro_scheduler_create();
        int counter = 0;

        check_not_null(sched);
        check_not_null(coro_spawn(sched, sched_fast, &counter, NULL));
        check_not_null(coro_spawn(sched, sched_fast, &counter, NULL));

        coro_scheduler_run(sched);
        check_int_eq(counter, 20);
        check_int_eq(coro_scheduler_count(sched), 0);

        coro_scheduler_destroy(sched);
    }

    it("calls discard callback during forced scheduler teardown") {
        coro_scheduler_t *sched = coro_scheduler_create();
        int times = 2;
        coro_t *co = NULL;

        check_not_null(sched);
        co = coro_spawn(sched, counter_coro, &times, NULL);
        check_not_null(co);
        coro_set_discard(co, discard_callback, &g_discard_count);

        coro_scheduler_destroy(sched);
        check_int_eq(g_discard_count, 1);
    }
}

spec("Turbo Coro Pool") {
    before_each() {
        g_counter = 0;
    }

    it("creates and destroys a generic coroutine pool") {
        turbo_coro_pool_config_t config = TURBO_CORO_POOL_CONFIG_DEFAULT;
        turbo_coro_pool_t *pool = turbo_coro_pool_create(&config);

        check_not_null(pool);
        check_int_eq(turbo_coro_pool_free_count(pool), 16);
        check_int_eq(turbo_coro_pool_active_count(pool), 0);
        check_int_eq(turbo_coro_pool_capacity(pool), 16);

        turbo_coro_pool_destroy(pool);
    }

    it("acquires and releases pooled coroutines") {
        turbo_coro_pool_config_t config = {.initial_capacity = 2, .max_capacity = 4, .stack_size = 0, .storage_size = 0};
        turbo_coro_pool_t *pool = turbo_coro_pool_create(&config);
        int counter = 0;
        coro_t *co1 = NULL;
        coro_t *co2 = NULL;

        check_not_null(pool);
        co1 = turbo_coro_pool_acquire(pool, sched_worker, &counter);
        co2 = turbo_coro_pool_acquire(pool, sched_worker, &counter);
        check_not_null(co1);
        check_not_null(co2);
        check_int_eq(turbo_coro_pool_active_count(pool), 2);
        check_int_eq(turbo_coro_pool_free_count(pool), 0);

        while (coro_alive(co1)) check_int_eq(coro_resume(co1), 0);
        while (coro_alive(co2)) check_int_eq(coro_resume(co2), 0);

        turbo_coro_pool_release(pool, co1);
        turbo_coro_pool_release(pool, co2);
        check_int_eq(turbo_coro_pool_active_count(pool), 0);
        check_int_eq(turbo_coro_pool_free_count(pool), 2);

        turbo_coro_pool_destroy(pool);
    }

    it("respects max capacity") {
        turbo_coro_pool_config_t config = {.initial_capacity = 1, .max_capacity = 2, .stack_size = 0, .storage_size = 0};
        turbo_coro_pool_t *pool = turbo_coro_pool_create(&config);
        int counter = 0;
        coro_t *co1 = NULL;
        coro_t *co2 = NULL;

        check_not_null(pool);
        co1 = turbo_coro_pool_acquire(pool, sched_fast, &counter);
        co2 = turbo_coro_pool_acquire(pool, sched_fast, &counter);
        check_not_null(co1);
        check_not_null(co2);
        check(turbo_coro_pool_acquire(pool, sched_fast, &counter) == NULL);

        while (coro_alive(co1)) check_int_eq(coro_resume(co1), 0);
        while (coro_alive(co2)) check_int_eq(coro_resume(co2), 0);
        turbo_coro_pool_release(pool, co1);
        turbo_coro_pool_release(pool, co2);

        turbo_coro_pool_destroy(pool);
    }

    it("reuses coroutine shells without overwriting user data") {
        turbo_coro_pool_config_t config = {.initial_capacity = 1, .max_capacity = 1, .stack_size = 0, .storage_size = 0};
        turbo_coro_pool_t *pool = turbo_coro_pool_create(&config);
        int c1 = 0;
        int c2 = 0;
        int user_data = 42;
        coro_t *co1 = NULL;
        coro_t *co2 = NULL;

        check_not_null(pool);
        co1 = turbo_coro_pool_acquire(pool, sched_worker, &c1);
        check_not_null(co1);
        coro_set_data(co1, &user_data);
        while (coro_alive(co1)) check_int_eq(coro_resume(co1), 0);
        turbo_coro_pool_release(pool, co1);

        co2 = turbo_coro_pool_acquire(pool, sched_worker, &c2);
        check_not_null(co2);
        check(co1 == co2);
        check(coro_get_data(co2) == &user_data);
        while (coro_alive(co2)) check_int_eq(coro_resume(co2), 0);
        turbo_coro_pool_release(pool, co2);

        check_int_eq(c1, 3);
        check_int_eq(c2, 3);
        turbo_coro_pool_destroy(pool);
    }

    it("spawns pooled coroutines into a scheduler") {
        turbo_coro_pool_config_t config = {.initial_capacity = 2, .max_capacity = 2, .stack_size = 0, .storage_size = 0};
        turbo_coro_pool_t *pool = turbo_coro_pool_create(&config);
        coro_scheduler_t *sched = coro_scheduler_create();
        int c1 = 0;
        int c2 = 0;

        check_not_null(pool);
        check_not_null(sched);
        check_not_null(turbo_coro_spawn_pooled(sched, pool, sched_worker, &c1));
        check_not_null(turbo_coro_spawn_pooled(sched, pool, sched_worker, &c2));
        check_int_eq(turbo_coro_pool_active_count(pool), 2);

        coro_scheduler_run(sched);
        check_int_eq(c1, 3);
        check_int_eq(c2, 3);
        check_int_eq(turbo_coro_pool_active_count(pool), 0);
        check_int_eq(turbo_coro_pool_free_count(pool), 2);

        coro_scheduler_destroy(sched);
        turbo_coro_pool_destroy(pool);
    }
}
