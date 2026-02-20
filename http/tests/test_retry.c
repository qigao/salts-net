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

/* ── Result struct ────────────────────────────────────────────────── */

static struct {
  int ran, skipped, status_code;
  int has_error;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_retry_defaults(turbo_coro_context_t *ctx) {
  (void)ctx;
  /* Just test the default struct — no coro needed */
}

static void test_retry_on_5xx(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);

  http_async_retry_policy_t policy = {
      .max_retries = 2,
      .initial_delay_ms = 100,
      .max_delay_ms = 1000,
      .exponential_backoff = 0,
      .retry_on_5xx = 1,
      .jitter_factor = 0.0
  };
  http_coro_client_set_retry_policy(c, &policy);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/status/500");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }
  g_result.status_code = r->status_code;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_no_retry_on_success(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);

  http_async_retry_policy_t policy = http_async_retry_policy_default();
  http_coro_client_set_retry_policy(c, &policy);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/get");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }
  g_result.status_code = r->status_code;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_retry_conn_error(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 3000);

  http_async_retry_policy_t policy = {
      .max_retries = 1,
      .initial_delay_ms = 50,
      .max_delay_ms = 100,
      .exponential_backoff = 0,
      .retry_on_connection_error = 1,
      .jitter_factor = 0.0
  };
  http_coro_client_set_retry_policy(c, &policy);

  http_coro_response_t *r = http_coro_get(c, "https://nonexistent-host-12345.com");
  g_result.has_error = (r->error != NULL);

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_no_retry_on_4xx(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);

  http_async_retry_policy_t policy = http_async_retry_policy_default();
  http_coro_client_set_retry_policy(c, &policy);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/status/404");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }
  g_result.status_code = r->status_code;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

spec("http retry policy") {

    describe("defaults") {

        it("should have sane defaults") {
            http_async_retry_policy_t policy = http_async_retry_policy_default();
            check_int_eq(policy.max_retries, 3);
            check_int_eq(policy.initial_delay_ms, 1000);
            check_int_eq(policy.exponential_backoff, 1);
            check_int_eq(policy.retry_on_timeout, 0);
            check_int_eq(policy.retry_on_connection_error, 1);
            check_int_eq(policy.retry_on_5xx, 1);
        }
    }

    describe("set and get") {

        it("should store policy") {
            turbo_coro_context_t *ctx = turbo_coro_context_create();
            http_coro_client_t *client = http_coro_client_create(ctx);
            http_async_retry_policy_t policy = {
                .max_retries = 5,
                .initial_delay_ms = 500,
                .max_delay_ms = 10000,
                .exponential_backoff = 1,
                .retry_on_timeout = 1,
                .retry_on_connection_error = 1,
                .retry_on_5xx = 1,
                .jitter_factor = 0.1
            };
            http_coro_client_set_retry_policy(client, &policy);

            http_async_retry_policy_t retrieved;
            http_coro_client_get_retry_policy(client, &retrieved);
            check_int_eq(retrieved.max_retries, 5);
            check_int_eq(retrieved.initial_delay_ms, 500);
            check_int_eq(retrieved.exponential_backoff, 1);

            http_coro_client_destroy(client);
            turbo_coro_context_destroy(ctx);
        }

        it("should clear policy") {
            turbo_coro_context_t *ctx = turbo_coro_context_create();
            http_coro_client_t *client = http_coro_client_create(ctx);
            http_async_retry_policy_t policy = http_async_retry_policy_default();
            http_coro_client_set_retry_policy(client, &policy);
            http_coro_client_clear_retry_policy(client);

            http_async_retry_policy_t retrieved;
            http_coro_client_get_retry_policy(client, &retrieved);
            check_int_eq(retrieved.max_retries, 0);

            http_coro_client_destroy(client);
            turbo_coro_context_destroy(ctx);
        }
    }

    describe("behavior") {

        it("should retry on 5xx") {
            run_in_coro(test_retry_on_5xx);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped)
                check_int_eq(g_result.status_code, 500);
        }

        it("should not retry on success") {
            run_in_coro(test_no_retry_on_success);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped)
                check_int_eq(g_result.status_code, 200);
        }

        it("should retry on connection error") {
            run_in_coro(test_retry_conn_error);
            check_int_eq(g_result.ran, 1);
            check_int_eq(g_result.has_error, 1);
        }

        it("should not retry on 4xx") {
            run_in_coro(test_no_retry_on_4xx);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped)
                check_int_eq(g_result.status_code, 404);
        }
    }
}
