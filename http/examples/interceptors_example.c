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
  int req_called, resp_called;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Interceptor callbacks ────────────────────────────────────────── */

static int s_req_called = 0;
static int s_resp_called = 0;

static int log_request(http_async_request_context_t *ctx) {
  (void)ctx;
  s_req_called++;
  return 0;
}

static void log_response(http_async_response_context_t *ctx) {
  (void)ctx;
  s_resp_called++;
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_interceptors(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;
  s_req_called = 0;
  s_resp_called = 0;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);
  http_coro_client_add_request_interceptor(client, log_request, NULL);
  http_coro_client_add_response_interceptor(client, log_response, NULL);

  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/get");
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;
  g_result.req_called = s_req_called;
  g_result.resp_called = s_resp_called;

  http_coro_response_free(response);
  http_coro_client_destroy(client);
}

static void test_clear_interceptors(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;
  s_req_called = 0;
  s_resp_called = 0;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);
  http_coro_client_add_request_interceptor(client, log_request, NULL);
  http_coro_client_add_response_interceptor(client, log_response, NULL);
  http_coro_client_clear_interceptors(client);

  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/get");
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;
  g_result.req_called = s_req_called;
  g_result.resp_called = s_resp_called;

  http_coro_response_free(response);
  http_coro_client_destroy(client);
}

spec("Interceptors Test") {

  it("should successfully use request and response interceptors") {
    run_in_coro(test_interceptors);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check(g_result.req_called >= 1);
      check(g_result.resp_called >= 1);
    }
  }

  it("should successfully clear interceptors") {
    run_in_coro(test_clear_interceptors);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check_int_eq(g_result.req_called, 0);
      check_int_eq(g_result.resp_called, 0);
    }
  }
}
