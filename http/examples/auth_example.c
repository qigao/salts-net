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
  int body_contains_auth, body_contains_token;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_basic_auth(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);
  http_coro_client_set_basic_auth(client, "user", "passwd");

  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/basic-auth/user/passwd");
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;
  g_result.body_contains_auth = (response->body && strstr(response->body, "\"authenticated\": true") != NULL);

  http_coro_response_free(response);
  http_coro_client_destroy(client);
}

static void test_bearer_auth(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);
  http_coro_client_set_bearer_token(client, "my-secret-token-12345");

  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/bearer");
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;
  g_result.body_contains_token = (response->body && strstr(response->body, "my-secret-token-12345") != NULL);

  http_coro_response_free(response);
  http_coro_client_destroy(client);
}

static void test_no_auth(turbo_coro_context_t *ctx) {
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

  http_coro_response_free(response);
  http_coro_client_destroy(client);
}

spec("Authentication Test") {

  it("should successfully perform Basic Authentication") {
    run_in_coro(test_basic_auth);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check_int_eq(g_result.body_contains_auth, 1);
    }
  }

  it("should successfully perform Bearer Token Authentication") {
    run_in_coro(test_bearer_auth);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check_int_eq(g_result.body_contains_token, 1);
    }
  }

  it("should successfully perform a request without authentication") {
    run_in_coro(test_no_auth);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
    }
  }
}
