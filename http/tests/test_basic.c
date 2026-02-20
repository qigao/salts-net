#include "tinytest.h"
#include "http_coro_client.h"
#include <turbo_coro.h>
#include <string.h>

/* ── Coro test harness ────────────────────────────────────────────── */

typedef struct { turbo_coro_context_t *ctx; void (*test_fn)(turbo_coro_context_t *ctx); } coro_test_ctx_t;

static void coro_test_entry(turbo_coro_t *co, void *arg) {
  UNUSED(co);
  coro_test_ctx_t *tctx = (coro_test_ctx_t *)arg;
  tctx->test_fn(tctx->ctx);
}

static void run_in_coro(void (*fn)(turbo_coro_context_t *ctx)) {
  turbo_coro_context_t *ctx = turbo_coro_context_create();
  coro_test_ctx_t tctx = {.ctx = ctx, .test_fn = fn};
  turbo_coro_scheduler_t *sched = turbo_coro_scheduler_create();
  turbo_coro_spawn(sched, coro_test_entry, &tctx);
  turbo_coro_scheduler_run(sched);
  turbo_coro_scheduler_destroy(sched);
  turbo_coro_context_destroy(ctx);
}

/* ── Coro test for malformed URL ──────────────────────────────────── */

static int g_malformed_ran = 0;

static int g_malformed_result = 0;

static void test_malformed_url(turbo_coro_context_t *ctx) {
  g_malformed_ran = 1;
  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_response_t *r = http_coro_get(c, "not-a-url");
  g_malformed_result = (r != NULL);
  if (r) http_coro_response_free(r);
  http_coro_client_destroy(c);
}

spec("http client basic") {

    describe("client lifecycle") {

        it("should create and destroy") {
            turbo_coro_context_t *ctx = turbo_coro_context_create();
            http_coro_client_t *client = http_coro_client_create(ctx);
            check_not_null(client);
            http_coro_client_destroy(client);
            turbo_coro_context_destroy(ctx);
        }

        it("should handle destroy NULL") {
            http_coro_client_destroy(NULL);
            check(1);
        }
    }

    describe("client configuration") {
        static turbo_coro_context_t *ctx;
        static http_coro_client_t *client;

        before_each() {
            ctx = turbo_coro_context_create();
            client = http_coro_client_create(ctx);
        }
        after_each() {
            http_coro_client_destroy(client);
            turbo_coro_context_destroy(ctx);
        }

        it("should set timeout") {
            check_not_null(client);
            http_coro_client_set_timeout(client, 5000);
            http_coro_client_set_timeout(client, 0);
            http_coro_client_set_timeout(client, -1);
            check(1);
        }

        it("should set user agent") {
            http_coro_client_set_user_agent(client, "TestAgent/1.0");
            http_coro_client_set_user_agent(client, "");
            http_coro_client_set_user_agent(client, NULL);
            check(1);
        }

        it("should set follow redirects") {
            http_coro_client_follow_redirects(client, 1);
            http_coro_client_follow_redirects(client, 0);
            check(1);
        }

        it("should set max redirects") {
            http_coro_client_set_max_redirects(client, 5);
            http_coro_client_set_max_redirects(client, 0);
            http_coro_client_set_max_redirects(client, 100);
            http_coro_client_set_max_redirects(client, -1);
            check(1);
        }
    }

    describe("error handling") {

        it("should handle response free NULL") {
            http_coro_response_free(NULL);
            check(1);
        }

        it("should return error for NULL url") {
            turbo_coro_context_t *ctx = turbo_coro_context_create();
            http_coro_client_t *client = http_coro_client_create(ctx);
            check_not_null(client);
            http_coro_client_destroy(client);
            turbo_coro_context_destroy(ctx);
        }

        it("should handle malformed url") {
            g_malformed_ran = 0;
            g_malformed_result = 0;
            run_in_coro(test_malformed_url);
            check_int_eq(g_malformed_ran, 1);
            check_int_eq(g_malformed_result, 1);
        }
    }
}
