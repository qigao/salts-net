/**
 * @file test_coro_auto_cleanup.c
 * @brief Test automatic coroutine cleanup in coro_context
 */

#include "CoroNet.h"
#include "turbo_coro.h"
#include "tinytest.h"
#include "platform.h"
#include <stdio.h>

static int g_coro_executed = 0;
static int g_coro_count = 0;
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

static int run_ctx_until(coro_context_t *ctx, const volatile int *value, int expected,
                         uint64_t timeout_ms) {
    uint64_t deadline;

    if (!ctx || !value) return -1;
    if (expected <= 0) return 0;

    deadline = turbo_monotonic_ms() + timeout_ms;
    while (*value < expected && turbo_monotonic_ms() < deadline) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    return (*value >= expected) ? 0 : -1;
}

/* ── Test coroutines ──────────────────────────────────────── */

static void simple_coro(coro_t *co, void *arg) {
    (void)co;
    int *counter = (int *)arg;
    (*counter)++;
    g_coro_executed++;
    /* Coroutine ends here, should be auto-cleaned */
}

static void yielding_coro(coro_t *co, void *arg) {
    (void)co;
    int *counter = (int *)arg;

    (*counter)++;
    coro_yield();

    (*counter)++;
    coro_yield();

    (*counter)++;
    /* Coroutine ends, should be auto-cleaned */
}

static void sleep_coro(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = (coro_context_t *)arg;

    g_coro_count++;
    coro_sleep(ctx, 10);  /* Sleep 10ms */
    g_coro_count++;

    /* Coroutine ends, should be auto-cleaned */
}

/* ── Echo server handler ──────────────────────────────────── */

static void echo_handler(coro_socket_t *client, void *arg) {
    (void)client;
    (void)arg;

    g_coro_executed++;
    /* Handler ends, client coroutine should be auto-cleaned */
}

/* ── Client task for testing ──────────────────────────────── */

static void server_client_task(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = (coro_context_t *)arg;

    /* Create and start server */
    coro_socket_t *server = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
    if (!server) return;

    int r = coro_socket_listen_on(server, "127.0.0.1", 19999, echo_handler, NULL);
    if (r != 0) {
        coro_socket_destroy(server);
        return;
    }

    /* Yield to let accept_loop_task start */
    coro_yield();

    /* Now connect client */
    coro_socket_t *client = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
    if (!client) {
        coro_socket_destroy(server);
        return;
    }

    r = coro_socket_connect(client, "127.0.0.1", 19999);
    check_equal(r, 0);

    coro_socket_destroy(client);

    /* Give the accepted handler coroutine a chance to run before listener teardown. */
    coro_sleep(ctx, 50);

    coro_socket_destroy(server);
}

/* ── Tests ────────────────────────────────────────────────── */

spec("Coroutine Auto-Cleanup") {

    it("should auto-clean coroutine that completes immediately") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        int counter = 0;
        g_coro_executed = 0;

        int r = coro_context_spawn(ctx, simple_coro, &counter);
        check_equal(r, 0);

        /* Run loop to execute coroutine */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);

        check_equal(counter, 1);
        check_equal(g_coro_executed, 1);

        robust_context_destroy(ctx);
    }

    it("should auto-clean multiple coroutines") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        int c1 = 0, c2 = 0, c3 = 0;
        g_coro_executed = 0;

        coro_context_spawn(ctx, simple_coro, &c1);
        coro_context_spawn(ctx, simple_coro, &c2);
        coro_context_spawn(ctx, simple_coro, &c3);

        /* Run loop to execute all coroutines */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);

        check_equal(c1, 1);
        check_equal(c2, 1);
        check_equal(c3, 1);
        check_equal(g_coro_executed, 3);

        robust_context_destroy(ctx);
    }

    it("should auto-clean yielding coroutines") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        int counter = 0;

        coro_context_spawn(ctx, yielding_coro, &counter);

        /* Run loop to execute and resume coroutine */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
        check_equal(counter, 1);  /* First increment before yield */

        /* Run again to resume after first yield */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
        check_equal(counter, 2);  /* Second increment */

        /* Run again to complete */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
        check_equal(counter, 3);  /* Final increment */

        robust_context_destroy(ctx);
    }

    it("should auto-clean coroutines with async I/O") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        g_coro_count = 0;

        /* Spawn 3 coroutines that sleep */
        coro_context_spawn(ctx, sleep_coro, ctx);
        coro_context_spawn(ctx, sleep_coro, ctx);
        coro_context_spawn(ctx, sleep_coro, ctx);

        /* Run until all three sleepers have resumed. */
        check_equal(run_ctx_until(ctx, &g_coro_count, 6, 5000), 0);

        check_equal(g_coro_count, 6);  /* All completed (3 start + 3 end) */

        robust_context_destroy(ctx);
    }

    it("should auto-clean server connection coroutines") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        g_coro_executed = 0;

        /* Spawn coroutine that creates server and client */
        coro_context_spawn(ctx, server_client_task, ctx);

        /* Run until the handler coroutine has completed. */
        check_equal(run_ctx_until(ctx, &g_coro_executed, 1, 5000), 0);

        robust_context_destroy(ctx);

        /* Server handler coroutine should have been auto-cleaned */
        check_equal(g_coro_executed, 1);
    }
}
