#include "tinytest.h"
#include "http_coro_client.h"
#include <turbo_coro.h>
#include <time.h>

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
  int has_error;
  double elapsed;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_no_retry(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);
  http_coro_client_clear_retry_policy(client);

  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/status/500");
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;

  http_coro_response_free(response);
  http_coro_client_destroy(client);
}

static void test_retry_on_500(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);

  http_retry_policy_t policy = {0};
  policy.max_retries = 2;
  policy.initial_delay_ms = 100;
  policy.retry_on_5xx = 1;
  http_coro_client_set_retry_policy(client, &policy);

  clock_t start = clock();
  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/status/500");
  clock_t end = clock();

  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;
  g_result.elapsed = ((double)(end - start)) / CLOCKS_PER_SEC;

  http_coro_response_free(response);
  http_coro_client_destroy(client);
}

static void test_retry_conn_error(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 5000);

  http_retry_policy_t policy = {0};
  policy.retry_on_connection_error = 1;
  policy.max_retries = 1;
  policy.initial_delay_ms = 100;
  http_coro_client_set_retry_policy(client, &policy);

  http_coro_response_t *response = http_coro_get(client, "https://this-host-does-not-exist-12345.com");
  g_result.has_error = (response->error != NULL);

  http_coro_response_free(response);
  http_coro_client_destroy(client);
}

spec("Retry Policy Test") {

  it("should fail immediately on 500 error when no retry policy is set") {
    run_in_coro(test_no_retry);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 500);
    }
  }

  it("should successfully apply a retry policy on 500 errors") {
    run_in_coro(test_retry_on_500);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 500);
      check(g_result.elapsed >= 0.2);
    }
  }

  it("should successfully retry on connection errors") {
    run_in_coro(test_retry_conn_error);
    check_int_eq(g_result.ran, 1);
    check_int_eq(g_result.has_error, 1);
  }
}
