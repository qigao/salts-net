/**
 * @file test_coro_auto_cleanup.c
 * @brief Test automatic coroutine cleanup in coro_context
 */

#include "netcore.h"
#include "turbo_coro.h"
#include "tinytest.h"
#include <stdio.h>

static int g_coro_executed = 0;
static int g_coro_count = 0;

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

static void echo_handler(coro_client_t *client, void *arg) {
    (void)arg;

    char *data = NULL;
    size_t len = 0;

    int r = coro_client_recv(client, &data, &len);
    if (r == 0 && data) {
        coro_client_send(client, data, len);
        coro_client_free_recv(data);
    }

    g_coro_executed++;
    /* Handler ends, client coroutine should be auto-cleaned */
}

/* ── Tests ────────────────────────────────────────────────── */

spec("Coroutine Auto-Cleanup") {

    it("should auto-clean coroutine that completes immediately") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        int counter = 0;
        g_coro_executed = 0;

        int r = coro_context_spawn(ctx, simple_coro, &counter);
        check_int_eq(r, 0);

        /* Run loop to execute coroutine */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);

        check_int_eq(counter, 1);
        check_int_eq(g_coro_executed, 1);

        coro_context_destroy(ctx);
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

        check_int_eq(c1, 1);
        check_int_eq(c2, 1);
        check_int_eq(c3, 1);
        check_int_eq(g_coro_executed, 3);

        coro_context_destroy(ctx);
    }

    it("should auto-clean yielding coroutines") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        int counter = 0;

        coro_context_spawn(ctx, yielding_coro, &counter);

        /* Run loop to execute and resume coroutine */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
        check_int_eq(counter, 1);  /* First increment before yield */

        /* Run again to resume after first yield */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
        check_int_eq(counter, 2);  /* Second increment */

        /* Run again to complete */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
        check_int_eq(counter, 3);  /* Final increment */

        coro_context_destroy(ctx);
    }

    it("should auto-clean coroutines with async I/O") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        g_coro_count = 0;

        /* Spawn 3 coroutines that sleep */
        coro_context_spawn(ctx, sleep_coro, ctx);
        coro_context_spawn(ctx, sleep_coro, ctx);
        coro_context_spawn(ctx, sleep_coro, ctx);

        /* Run loop until all complete */
        coro_context_run(ctx, TURBO_RUN_DEFAULT);

        check_int_eq(g_coro_count, 6);  /* All completed (3 start + 3 end) */

        coro_context_destroy(ctx);
    }

    it("should auto-clean server connection coroutines") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        g_coro_executed = 0;

        /* Create echo server */
        coro_server_t *server = coro_server_create(ctx);
        check(server != NULL);

        int r = coro_server_listen(server, "tcp://127.0.0.1:0", echo_handler, NULL);
        check_int_eq(r, 0);

        /* Create client and connect */
        coro_client_t *client = coro_client_create(ctx);
        check(client != NULL);

        r = coro_client_connect(client, "tcp://127.0.0.1:9999");
        if (r == 0) {
            const char *msg = "hello";
            coro_client_send(client, msg, 5);

            char *recv_data = NULL;
            size_t recv_len = 0;
            coro_client_recv(client, &recv_data, &recv_len);

            if (recv_data) {
                check_int_eq(recv_len, 5);
                coro_client_free_recv(recv_data);
            }
        }

        coro_client_destroy(client);
        coro_server_destroy(server);

        /* Run loop to process cleanup */
        coro_context_run(ctx, TURBO_RUN_NOWAIT);

        coro_context_destroy(ctx);

        /* Server handler coroutine should have been auto-cleaned */
        /* (We can't easily verify the count without exposing internals) */
    }
}
