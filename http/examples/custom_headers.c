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
  int body_has_myvalue, body_has_12345, body_has_ua;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test function ───────────────────────────────────────────── */

static void test_custom_headers(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);
  http_coro_client_set_user_agent(client, "MyApp/2.0");

  const char *headers[] = {
      "X-Custom-Header: MyValue",
      "X-Test-Request-ID: 12345",
      "Accept: application/json"
  };

  http_coro_response_t *response = http_coro_request(client, HTTP_GET,
      "https://httpbin.org/headers", headers, 3, NULL, 0);
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;
  g_result.body_has_myvalue = (response->body && strstr(response->body, "MyValue") != NULL);
  g_result.body_has_12345 = (response->body && strstr(response->body, "12345") != NULL);
  g_result.body_has_ua = (response->body && strstr(response->body, "MyApp/2.0") != NULL);

  http_coro_response_free(response);
  http_coro_client_destroy(client);
}

spec("Custom Headers Test") {

  it("should successfully send custom headers") {
    run_in_coro(test_custom_headers);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check_int_eq(g_result.body_has_myvalue, 1);
      check_int_eq(g_result.body_has_12345, 1);
      check_int_eq(g_result.body_has_ua, 1);
    }
  }
}
