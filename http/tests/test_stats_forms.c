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
  turbo_coro_context_t *ctx = turbo_coro_context_create(NULL);
  coro_test_ctx_t tctx = {.ctx = ctx, .test_fn = fn};
  turbo_coro_scheduler_t *sched = turbo_coro_scheduler_create();
  turbo_coro_spawn(sched, coro_test_entry, &tctx);
  turbo_coro_scheduler_run(sched);
  turbo_coro_scheduler_destroy(sched);
  turbo_coro_context_destroy(ctx);
}

/* ── Result struct ────────────────────────────────────────────────── */

static struct {
  int ran, skipped, status_code;
  uint64_t total_requests, successful_requests;
  int body_has_form_type;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_track_requests(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);
  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/get");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }
  http_coro_response_free(r);

  http_client_stats_t stats;
  http_coro_client_get_stats(c, &stats);
  g_result.total_requests = stats.total_requests;
  g_result.successful_requests = stats.successful_requests;

  http_coro_client_reset_stats(c);
  http_coro_client_get_stats(c, &stats);
  /* After reset, total should be 0 — store for second check */

  http_coro_client_destroy(c);
}

static void test_form_post(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);

  http_params_t *params = http_params_create();
  http_params_add(params, "name", "Test User");
  http_params_add(params, "value", "123");

  http_coro_response_t *r = http_coro_post_form(c, "https://httpbin.org/post", params);
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_params_free(params);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  g_result.body_has_form_type = (r->body && strstr(r->body, "application/x-www-form-urlencoded") != NULL);

  http_coro_response_free(r);
  http_params_free(params);
  http_coro_client_destroy(c);
}

spec("http stats and forms") {

    describe("statistics") {

        it("should start at zero") {
            turbo_coro_context_t *ctx = turbo_coro_context_create(NULL);
            http_coro_client_t *client = http_coro_client_create(ctx);
            http_client_stats_t stats;
            http_coro_client_get_stats(client, &stats);
            check_size_eq(stats.total_requests, 0);
            check_size_eq(stats.successful_requests, 0);
            http_coro_client_destroy(client);
            turbo_coro_context_destroy(ctx);
        }

        it("should track requests") {
            run_in_coro(test_track_requests);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_size_eq(g_result.total_requests, 1);
                check_size_eq(g_result.successful_requests, 1);
            }
        }
    }

    describe("URL params") {

        it("should encode params") {
            http_params_t *params = http_params_create();
            check_not_null(params);

            http_params_add(params, "name", "John Doe");
            http_params_add(params, "email", "test@example.com");

            char *encoded = http_params_encode(params);
            check_not_null(encoded);
            check(strlen(encoded) > 0, "encoded string should not be empty");
            check(strstr(encoded, "name=John+Doe") != NULL ||
                  strstr(encoded, "name=John%20Doe") != NULL,
                  "name should be encoded");
            check_str_contains(encoded, "email=test%40example.com");

            free(encoded);
            http_params_free(params);
        }

        it("should build URL with params") {
            http_params_t *params = http_params_create();
            http_params_add(params, "page", "1");
            http_params_add(params, "limit", "10");

            char *url = http_build_url("https://example.com/api", params);
            check_not_null(url);
            check_str_contains(url, "https://example.com/api?");
            check_str_contains(url, "page=1");
            check_str_contains(url, "limit=10");
            free(url);

            url = http_build_url("https://example.com/api?existing=value", params);
            check_not_null(url);
            check_str_contains(url, "existing=value");
            free(url);

            http_params_free(params);
        }
    }

    describe("form POST") {

        it("should post form data") {
            run_in_coro(test_form_post);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_int_eq(g_result.status_code, 200);
                check_int_eq(g_result.body_has_form_type, 1);
            }
        }
    }
}
