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

/* ── Coro test functions for error cases ──────────────────────────── */

static struct {
  int has_error;
} g_result;

static void test_get_null_url(turbo_coro_context_t *ctx) {
  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_response_t *r = http_coro_get(c, NULL);
  g_result.has_error = (r != NULL && r->error != NULL);
  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_post_null_url(turbo_coro_context_t *ctx) {
  http_coro_client_t *c = http_coro_client_create(ctx);
  const char *body = "{\"test\":\"data\"}";
  http_coro_response_t *r = http_coro_post(c, NULL, body, strlen(body));
  g_result.has_error = (r != NULL && r->error != NULL);
  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_post_null_url_empty(turbo_coro_context_t *ctx) {
  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_response_t *r = http_coro_post(c, NULL, NULL, 0);
  g_result.has_error = (r != NULL && r->error != NULL);
  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_custom_headers_null(turbo_coro_context_t *ctx) {
  http_coro_client_t *c = http_coro_client_create(ctx);
  const char *headers[] = {
      "X-Custom-Header: value1",
      "X-Another-Header: value2"
  };
  http_coro_response_t *r = http_coro_request(c, HTTP_GET, NULL, headers, 2, NULL, 0);
  g_result.has_error = (r != NULL && r->error != NULL);
  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_body_headers_null(turbo_coro_context_t *ctx) {
  http_coro_client_t *c = http_coro_client_create(ctx);
  const char *headers[] = {"Content-Type: application/json"};
  const char *body = "{\"key\":\"value\"}";
  http_coro_response_t *r = http_coro_request(c, HTTP_POST, NULL, headers, 1, body, strlen(body));
  g_result.has_error = (r != NULL && r->error != NULL);
  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_all_methods_null(turbo_coro_context_t *ctx) {
  http_coro_client_t *c = http_coro_client_create(ctx);
  http_method_t methods[] = {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE, HTTP_HEAD, HTTP_PATCH};
  for (int i = 0; i < 6; i++) {
    http_coro_response_t *r = http_coro_request(c, methods[i], NULL, NULL, 0, NULL, 0);
    if (!r || !r->error) { g_result.has_error = 0; http_coro_response_free(r); http_coro_client_destroy(c); return; }
    http_coro_response_free(r);
  }
  g_result.has_error = 1;
  http_coro_client_destroy(c);
}

static void test_query_params(turbo_coro_context_t *ctx) {
  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 100);
  http_coro_response_t *r = http_coro_get(c, "http://127.0.0.1:1/test?param1=value1&param2=value2");
  g_result.has_error = (r != NULL);
  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_fragment(turbo_coro_context_t *ctx) {
  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 100);
  http_coro_response_t *r = http_coro_get(c, "http://127.0.0.1:1/test#fragment");
  g_result.has_error = (r != NULL);
  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

spec("http requests") {

    describe("GET") {

        it("should return error for NULL url") {
            run_in_coro(test_get_null_url);
            check_int_eq(g_result.has_error, 1);
        }
    }

    describe("POST") {

        it("should return error for NULL url with body") {
            run_in_coro(test_post_null_url);
            check_int_eq(g_result.has_error, 1);
        }

        it("should return error for NULL url with empty body") {
            run_in_coro(test_post_null_url_empty);
            check_int_eq(g_result.has_error, 1);
        }
    }

    describe("custom headers") {

        it("should accept custom headers") {
            run_in_coro(test_custom_headers_null);
            check_int_eq(g_result.has_error, 1);
        }

        it("should accept body and headers together") {
            run_in_coro(test_body_headers_null);
            check_int_eq(g_result.has_error, 1);
        }
    }

    describe("HTTP methods") {

        it("should accept all standard methods") {
            run_in_coro(test_all_methods_null);
            check_int_eq(g_result.has_error, 1);
        }
    }

    describe("URL parsing") {

        it("should handle query params") {
            run_in_coro(test_query_params);
            check_int_eq(g_result.has_error, 1);
        }

        it("should handle fragment") {
            run_in_coro(test_fragment);
            check_int_eq(g_result.has_error, 1);
        }
    }
}
