#include <platform.h>
#include "tinytest.h"
#include "http_client.h"
#include <turbo_coro.h>
#include <netcore.h>
#include <string.h>
#include <stdlib.h>

/* ── Coro test harness ────────────────────────────────────────────── */

typedef struct {
    coro_context_t *ctx;
    void (*test_fn)(coro_context_t *ctx);
} coro_test_ctx_t;

static void coro_test_entry(coro_t *co, void *arg) {
    UNUSED(co);
    coro_test_ctx_t *tctx = (coro_test_ctx_t *)arg;
    tctx->test_fn(tctx->ctx);
}

static void run_in_coro(void (*fn)(coro_context_t *ctx)) {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_test_ctx_t tctx = {.ctx = ctx, .test_fn = fn};
    coro_scheduler_t *sched = coro_scheduler_create();
    coro_spawn(sched, coro_test_entry, &tctx, NULL);
    coro_scheduler_run(sched);
    coro_scheduler_destroy(sched);
    coro_context_destroy(ctx);
}

/* ── Coro batch test ──────────────────────────────────────────────── */

static int g_coro_batch_ok = 0;

static void test_batch_in_coro(coro_context_t *ctx) {
    UNUSED(ctx);
    g_coro_batch_ok = 0;
    http_client_t *c = http_client_create();
    http_batch_request_t reqs[] = {
        {.method = HTTP_GET, .url = "http://invalid1.test.local"},
        {.method = HTTP_GET, .url = "http://invalid2.test.local"},
    };
    http_batch_result_t *results = http_client_batch(c, reqs, 2, 2);
    if (results && results[0].response && results[1].response)
        g_coro_batch_ok = 1;
    http_batch_result_free(results, 2);
    http_client_destroy(c);
}

/* ── Tests ────────────────────────────────────────────────────────── */

spec("http client batch") {

    describe("batch API guards") {

        it("should return NULL for NULL client") {
            http_batch_request_t req = {.method = HTTP_GET, .url = "http://example.com"};
            http_batch_result_t *r = http_client_batch(NULL, &req, 1, 1);
            check(r == NULL);
        }

        it("should return NULL for NULL requests") {
            http_client_t *c = http_client_create();
            http_batch_result_t *r = http_client_batch(c, NULL, 1, 1);
            check(r == NULL);
            http_client_destroy(c);
        }

        it("should return NULL for zero count") {
            http_client_t *c = http_client_create();
            http_batch_request_t req = {.method = HTTP_GET, .url = "http://example.com"};
            http_batch_result_t *r = http_client_batch(c, &req, 0, 1);
            check(r == NULL);
            http_client_destroy(c);
        }

        it("should return NULL for negative count") {
            http_client_t *c = http_client_create();
            http_batch_request_t req = {.method = HTTP_GET, .url = "http://example.com"};
            http_batch_result_t *r = http_client_batch(c, &req, -1, 1);
            check(r == NULL);
            http_client_destroy(c);
        }

        it("should handle free NULL results") {
            http_batch_result_free(NULL, 5);
            check(1);
        }
    }

    describe("batch execution outside coroutine") {

        it("should execute single request") {
            http_client_t *c = http_client_create();
            http_batch_request_t req = {
                .method = HTTP_GET, .url = "http://invalid.test.local",
                .body = NULL, .body_len = 0
            };
            http_batch_result_t *results = http_client_batch(c, &req, 1, 1);
            check_not_null(results);
            check_not_null(results[0].response);
            check(results[0].response->error_code != HTTP_ERROR_NONE);
            http_batch_result_free(results, 1);
            http_client_destroy(c);
        }

        it("should execute multiple requests with concurrency") {
            http_client_t *c = http_client_create();
            http_batch_request_t reqs[] = {
                {.method = HTTP_GET, .url = "http://invalid1.test.local"},
                {.method = HTTP_GET, .url = "http://invalid2.test.local"},
                {.method = HTTP_GET, .url = "http://invalid3.test.local"},
            };
            http_batch_result_t *results = http_client_batch(c, reqs, 3, 2);
            check_not_null(results);
            for (int i = 0; i < 3; i++) {
                check_not_null(results[i].response);
            }
            http_batch_result_free(results, 3);
            http_client_destroy(c);
        }

        it("should clamp concurrency to count") {
            http_client_t *c = http_client_create();
            http_batch_request_t req = {
                .method = HTTP_GET, .url = "http://invalid.test.local"
            };
            http_batch_result_t *results = http_client_batch(c, &req, 1, 100);
            check_not_null(results);
            check_not_null(results[0].response);
            http_batch_result_free(results, 1);
            http_client_destroy(c);
        }

        it("should default concurrency to 1 when zero") {
            http_client_t *c = http_client_create();
            http_batch_request_t req = {
                .method = HTTP_GET, .url = "http://invalid.test.local"
            };
            http_batch_result_t *results = http_client_batch(c, &req, 1, 0);
            check_not_null(results);
            check_not_null(results[0].response);
            http_batch_result_free(results, 1);
            http_client_destroy(c);
        }
    }

    describe("batch execution inside coroutine") {

        it("should execute batch inside a coroutine") {
            g_coro_batch_ok = 0;
            run_in_coro(test_batch_in_coro);
            check_int_eq(g_coro_batch_ok, 1);
        }
    }
}
