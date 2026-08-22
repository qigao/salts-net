/**
 * @file test_lazy_task.c
 * @brief Test lazy task API (deferred execution)
 */

#include "CoroNet.h" 
#include "turbo_coro.h"
#include "tinytest.h"
#include <stdio.h>

static int g_executed = 0;
static int g_counter = 0;
#include "turbo_coro_internal.h"

static void robust_context_destroy(coro_context_t *ctx) {
    if (!ctx) return;
    int max_drain = 500;
    while (max_drain-- > 0) {
        int has_handles = coro_context_alive(ctx);
        int has_coros = ctx->scheduler ? coro_scheduler_count(ctx->scheduler) > 0 : 0;
        if (!has_handles && !has_coros) break;
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
    }
    coro_context_destroy(ctx);
}

/* ── Test coroutines ──────────────────────────────────────── */

static void simple_task(coro_t *co, void *arg) {
    (void)co;
    int *counter = (int *)arg;
    (*counter)++;
    g_executed++;
}

static void sleep_task(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = (coro_context_t *)arg;
    g_executed++;
    coro_sleep(ctx, 10);
    g_executed++;
}

static void failing_task(coro_t *co, void *arg) {
    (void)co;
    (void)arg;
    g_executed = -1;  /* Should not execute if cancelled */
}

/* ── Tests ────────────────────────────────────────────────── */

spec("Lazy Task API") {

    it("should create task without executing") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        int counter = 0;
        g_executed = 0;

        /* Create task - should NOT execute */
        coro_task_t *task = coro_task_create(ctx, simple_task, &counter);
        check(task != NULL);
        check_equal(counter, 0);
        check_equal(g_executed, 0);

        /* Verify task is not done */
        check_equal(coro_task_is_done(task), 0);

        robust_context_destroy(ctx);
    }

    it("should execute task when started") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        int counter = 0;
        g_executed = 0;

        /* Create and start task */
        coro_task_t *task = coro_task_create(ctx, simple_task, &counter);
        check(task != NULL);

        int r = coro_task_start(task);
        check_equal(r, 0);

        /* Run loop to execute task */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);

        check_equal(counter, 1);
        check_equal(g_executed, 1);

        /* Task should be done */
        check_equal(coro_task_is_done(task), 1);

        robust_context_destroy(ctx);
    }

    it("should cancel task before start") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        g_executed = 0;

        /* Create task */
        coro_task_t *task = coro_task_create(ctx, failing_task, NULL);
        check(task != NULL);

        /* Cancel before start */
        int r = coro_task_cancel(task);
        check_equal(r, 0);

        /* Task should be done (cancelled) */
        check_equal(coro_task_is_done(task), 1);

        /* Try to start - should fail */
        r = coro_task_start(task);
        check_equal(r, TURBO_EINVAL);

        /* Verify task never executed */
        check_equal(g_executed, 0);

        /* Run loop to clean up */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);

        robust_context_destroy(ctx);
    }

    it("should not allow double start") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        int counter = 0;

        coro_task_t *task = coro_task_create(ctx, simple_task, &counter);
        check(task != NULL);

        /* Start once */
        int r = coro_task_start(task);
        check_equal(r, 0);

        /* Try to start again - should fail */
        r = coro_task_start(task);
        check_equal(r, TURBO_EINVAL);

        /* Run loop to execute task */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);

        check_equal(counter, 1);  /* Should execute only once */

        robust_context_destroy(ctx);
    }

    it("should handle conditional execution") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        int c1 = 0, c2 = 0, c3 = 0;

        /* Create three tasks */
        coro_task_t *t1 = coro_task_create(ctx, simple_task, &c1);
        coro_task_t *t2 = coro_task_create(ctx, simple_task, &c2);
        coro_task_t *t3 = coro_task_create(ctx, simple_task, &c3);

        check(t1 != NULL);
        check(t2 != NULL);
        check(t3 != NULL);

        /* Conditionally start tasks */
        coro_task_start(t1);      /* Start */
        coro_task_cancel(t2);     /* Cancel */
        coro_task_start(t3);      /* Start */

        /* Run loop to execute started tasks */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);

        check_equal(c1, 1);  /* Executed */
        check_equal(c2, 0);  /* Cancelled */
        check_equal(c3, 1);  /* Executed */

        robust_context_destroy(ctx);
    }

    it("should handle async tasks") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        g_executed = 0;

        /* Create task that sleeps */
        coro_task_t *task = coro_task_create(ctx, sleep_task, ctx);
        check(task != NULL);

        /* Start task */
        coro_task_start(task);

        /* Task should not be done yet (not even started) */
        check_equal(coro_task_is_done(task), 0);

        /* Run loop until completion */
        coro_context_run(ctx, TURBO_RUN_DEFAULT);

        check_equal(g_executed, 2);  /* Started and completed */

        robust_context_destroy(ctx);
    }

    it("should compare eager vs lazy execution") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        int eager_counter = 0;
        int lazy_counter = 0;

        /* Eager: spawns immediately but needs event loop to execute */
        coro_context_spawn(ctx, simple_task, &eager_counter);
        check_equal(eager_counter, 0);  /* Not executed yet */

        /* Lazy: does not execute until started */
        coro_task_t *task = coro_task_create(ctx, simple_task, &lazy_counter);
        check_equal(lazy_counter, 0);  /* Not executed yet */

        coro_task_start(task);
        check_equal(lazy_counter, 0);  /* Still not executed */

        /* Run loop to execute both */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);

        check_equal(eager_counter, 1);  /* Now executed */
        check_equal(lazy_counter, 1);   /* Now executed */

        robust_context_destroy(ctx);
    }

    it("should auto-cleanup completed tasks") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        g_counter = 0;

        /* Create and start multiple tasks */
        for (int i = 0; i < 10; i++) {
            coro_task_t *task = coro_task_create(ctx, simple_task, &g_counter);
            coro_task_start(task);
        }

        /* Run loop to execute and cleanup tasks */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);

        check_equal(g_counter, 10);

        /* All tasks should be cleaned up (we can't verify count without exposing internals) */

        robust_context_destroy(ctx);
    }

    it("should reclaim a started task after its owner releases the handle") {
        coro_context_t *ctx = coro_context_create(NULL);
        int counter = 0;
        check_not_null(ctx);

        coro_task_t *task = coro_task_create(ctx, simple_task, &counter);
        check_not_null(task);
        check_equal(coro_task_start(task), TURBO_OK);

        coro_task_destroy(task);
        (void)coro_context_run(ctx, TURBO_RUN_NOWAIT);

        check_equal(counter, 1);
        check_equal(turbo_vec_size(&ctx->tasks), 0);
        robust_context_destroy(ctx);
    }
}
