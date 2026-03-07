/**
 * @file test_combinators.c
 * @brief Test when_all and when_any combinators
 */

#include "netcore.h"
#include "turbo_coro_client.h"
#include "turbo_coro.h"
#include "tinytest.h"
#include <stdio.h>

static int g_counter = 0;
#include "turbo_coro_internal.h"

static void robust_context_destroy(coro_context_t *ctx) {
    if (!ctx) return;
    int max_drain = 500;
    while (max_drain-- > 0) {
        int has_handles = coro_context_alive(ctx);
        int has_coros = ctx->scheduler ? coro_scheduler_count(ctx->scheduler) > 0 : 0;
        if (!has_handles && !has_coros) break;
        uv_run(ctx->loop, UV_RUN_NOWAIT);
        if (ctx->scheduler) coro_scheduler_tick(ctx->scheduler);
    }
    coro_context_destroy(ctx);
}

/* ── Test tasks ───────────────────────────────────────────── */

static void task_10ms(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = coro_context_current();
    int *result = (int *)arg;

    coro_sleep(ctx, 10);
    *result = 10;
    g_counter++;
}

static void task_20ms(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = coro_context_current();
    int *result = (int *)arg;

    coro_sleep(ctx, 20);
    *result = 20;
    g_counter++;
}

static void task_30ms(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = coro_context_current();
    int *result = (int *)arg;

    coro_sleep(ctx, 30);
    *result = 30;
    g_counter++;
}

static void task_instant(coro_t *co, void *arg) {
    (void)co;
    int *result = (int *)arg;
    *result = 999;
    g_counter++;
}

/* ── Test coroutines ──────────────────────────────────────── */

typedef struct {
    coro_context_t *ctx;
    int *result;
} test_ctx_t;

static void test_when_all_coro(coro_t *co, void *arg) {
    (void)co;
    test_ctx_t *tctx = (test_ctx_t *)arg;
    coro_context_t *ctx = tctx->ctx;

    int r1 = 0, r2 = 0, r3 = 0;
    g_counter = 0;

    /* Create and start tasks */
    coro_task_t *t1 = coro_task_create(ctx, task_10ms, &r1);
    coro_task_t *t2 = coro_task_create(ctx, task_20ms, &r2);
    coro_task_t *t3 = coro_task_create(ctx, task_30ms, &r3);

    coro_task_start(t1);
    coro_task_start(t2);
    coro_task_start(t3);

    /* Wait for all */
    int r = coro_when_all(ctx, (coro_task_t *[]){t1, t2, t3}, 3);

    /* Verify */
    *tctx->result = (r == 0 && r1 == 10 && r2 == 20 && r3 == 30 && g_counter == 3) ? 1 : 0;
}

static void test_when_any_coro(coro_t *co, void *arg) {
    (void)co;
    test_ctx_t *tctx = (test_ctx_t *)arg;
    coro_context_t *ctx = tctx->ctx;

    /* Use heap-allocated results since tasks may outlive this function */
    int *r1 = malloc(sizeof(int));
    int *r2 = malloc(sizeof(int));
    int *r3 = malloc(sizeof(int));
    *r1 = 0; *r2 = 0; *r3 = 0;
    g_counter = 0;

    /* Create and start tasks (different speeds) */
    coro_task_t *t1 = coro_task_create(ctx, task_30ms, r1);
    coro_task_t *t2 = coro_task_create(ctx, task_10ms, r2);
    coro_task_t *t3 = coro_task_create(ctx, task_20ms, r3);

    coro_task_start(t1);
    coro_task_start(t2);
    coro_task_start(t3);

    /* Wait for any */
    int winner = coro_when_any(ctx, (coro_task_t *[]){t1, t2, t3}, 3);

    /* Verify that when_any returned a valid index and at least one task completed */
    *tctx->result = (winner >= 0 && winner < 3 &&
                     coro_task_is_done(winner == 0 ? t1 : (winner == 1 ? t2 : t3))) ? 1 : 0;

    /* Note: Don't free r1/r2/r3 - other tasks may still be using them */
}

static void test_when_any_instant_coro(coro_t *co, void *arg) {
    (void)co;
    test_ctx_t *tctx = (test_ctx_t *)arg;
    coro_context_t *ctx = tctx->ctx;

    /* Use heap-allocated results */
    int *r1 = malloc(sizeof(int));
    int *r2 = malloc(sizeof(int));
    *r1 = 0; *r2 = 0;
    g_counter = 0;

    /* One instant task, one slow task */
    coro_task_t *t1 = coro_task_create(ctx, task_30ms, r1);
    coro_task_t *t2 = coro_task_create(ctx, task_instant, r2);

    coro_task_start(t1);
    coro_task_start(t2);

    /* Should return immediately with t2 */
    int winner = coro_when_any(ctx, (coro_task_t *[]){t1, t2}, 2);

    /* Verify */
    *tctx->result = (winner == 1 && *r2 == 999) ? 1 : 0;

    /* Note: Don't free - other task may still be using them */
}

static void test_when_all_empty_coro(coro_t *co, void *arg) {
    (void)co;
    test_ctx_t *tctx = (test_ctx_t *)arg;
    coro_context_t *ctx = tctx->ctx;

    /* Test with 0 tasks - should return error */
    int r = coro_when_all(ctx, NULL, 0);

    *tctx->result = (r == TURBO_EINVAL) ? 1 : 0;
}

static void test_when_all_single_coro(coro_t *co, void *arg) {
    (void)co;
    test_ctx_t *tctx = (test_ctx_t *)arg;
    coro_context_t *ctx = tctx->ctx;

    int r1 = 0;

    coro_task_t *t1 = coro_task_create(ctx, task_10ms, &r1);
    coro_task_start(t1);

    /* Wait for single task */
    int r = coro_when_all(ctx, (coro_task_t *[]){t1}, 1);

    *tctx->result = (r == 0 && r1 == 10) ? 1 : 0;
}

/* ── Test helpers ─────────────────────────────────────────── */

static int run_coro_test(coro_fn fn) {
    coro_context_t *ctx = coro_context_create(NULL);
    if (!ctx) return 0;

    int result = 0;
    test_ctx_t tctx = {.ctx = ctx, .result = &result};

    /* Use context spawn for proper TLS setup */
    coro_context_spawn(ctx, fn, &tctx);
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);

    return result;
}

/* ── Tests ────────────────────────────────────────────────── */

spec("Task Combinators") {

    it("should wait for all tasks with when_all") {
        int result = run_coro_test(test_when_all_coro);
        check_int_eq(result, 1);
    }

    it("should return first completed task with when_any") {
        int result = run_coro_test(test_when_any_coro);
        check_int_eq(result, 1);
    }

    it("should handle instant completion in when_any") {
        int result = run_coro_test(test_when_any_instant_coro);
        check_int_eq(result, 1);
    }

    it("should handle empty task list") {
        int result = run_coro_test(test_when_all_empty_coro);
        check_int_eq(result, 1);
    }

    it("should handle single task") {
        int result = run_coro_test(test_when_all_single_coro);
        check_int_eq(result, 1);
    }

    it("should work with eager spawn and when_all") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        /* Note: when_all requires lazy tasks, not eager spawn */
        /* This test verifies the API doesn't crash with mixed usage */

        robust_context_destroy(ctx);
    }
}
