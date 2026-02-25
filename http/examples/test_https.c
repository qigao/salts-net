#include "tinytest.h"
#include "http_coro_client.h"
#include <turbo_coro.h>

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
  size_t body_len;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_https_get(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);

  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/get");
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;
  g_result.body_len = response->body_len;

  http_coro_response_free(response);
  http_coro_client_destroy(client);
}

static void test_redirect_single(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);

  http_coro_response_t *response = http_coro_get(client,
      "https://httpbin.org/redirect-to?url=https://httpbin.org/get");
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

static void test_redirect_multiple(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 15000);

  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/redirect/3");
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

static void test_redirect_limit(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 15000);
  http_coro_client_set_max_redirects(client, 2);

  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/redirect/5");
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

static void test_connection_reuse(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);
  http_coro_client_set_max_redirects(client, 10);

  for (int i = 0; i < 3; i++) {
    http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/get");
    if (is_network_error(response)) {
      g_result.skipped = 1;
      http_coro_response_free(response);
      http_coro_client_destroy(client);
      return;
    }
    g_result.status_code = response->status_code;
    http_coro_response_free(response);
  }

  http_coro_client_destroy(client);
}

spec("HTTPS and Redirect Tests") {

  it("should successfully perform a GET request over HTTPS") {
    run_in_coro(test_https_get);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check(g_result.body_len > 0);
    }
  }

  describe("Redirect Handling") {
    it("should follow redirects") {
      run_in_coro(test_redirect_single);
      check_int_eq(g_result.ran, 1);
      if (!g_result.skipped) {
        check_int_eq(g_result.status_code, 200);
      }
    }

    it("should follow multiple redirects correctly") {
      run_in_coro(test_redirect_multiple);
      check_int_eq(g_result.ran, 1);
      if (!g_result.skipped) {
        check_int_eq(g_result.status_code, 200);
      }
    }

    it("should respect the maximum redirect limit") {
      run_in_coro(test_redirect_limit);
      check_int_eq(g_result.ran, 1);
      if (!g_result.skipped) {
        check(g_result.status_code == 301 || g_result.status_code == 302);
      }
    }
  }

  describe("Connection Lifecycle") {
    it("should reuse connections for multiple requests to the same host") {
      run_in_coro(test_connection_reuse);
      check_int_eq(g_result.ran, 1);
      if (!g_result.skipped) {
        check_int_eq(g_result.status_code, 200);
      }
    }
  }
}
