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
  int has_body, is_json;
  size_t body_len;
  uint64_t total_requests, successful_requests, bytes_sent, bytes_received;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_coro_get(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);
  http_coro_client_set_user_agent(client, "CoroHTTP-Example/1.0");

  http_coro_response_t *resp = http_coro_get(client, "https://httpbin.org/get");
  if (is_network_error(resp)) {
    g_result.skipped = 1;
    http_coro_response_free(resp);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = resp->status_code;
  g_result.has_body = (resp->body != NULL);
  g_result.body_len = resp->body_len;

  http_coro_response_free(resp);
  http_coro_client_destroy(client);
}

static void test_coro_post_json(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);

  const char *json = "{\"message\": \"hello from coroutine\", \"version\": 1}";
  http_coro_response_t *resp = http_coro_post_json(client, "https://httpbin.org/post", json);
  if (is_network_error(resp)) {
    g_result.skipped = 1;
    http_coro_response_free(resp);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = resp->status_code;
  g_result.has_body = (resp->body != NULL);
  g_result.is_json = http_coro_response_is_json(resp);

  http_coro_response_free(resp);
  http_coro_client_destroy(client);
}

static void test_coro_bearer_auth(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);
  http_coro_client_set_bearer_token(client, "test-token-123");

  http_coro_response_t *resp = http_coro_get(client, "https://httpbin.org/bearer");
  if (is_network_error(resp)) {
    g_result.skipped = 1;
    http_coro_response_free(resp);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = resp->status_code;
  g_result.has_body = (resp->body != NULL && strstr(resp->body, "test-token-123") != NULL);

  http_coro_response_free(resp);
  http_coro_client_clear_auth(client);
  http_coro_client_destroy(client);
}

static void test_coro_stats(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);
  http_coro_client_reset_stats(client);

  http_coro_response_t *resp = http_coro_get(client, "https://httpbin.org/get");
  if (is_network_error(resp)) {
    g_result.skipped = 1;
    http_coro_response_free(resp);
    http_coro_client_destroy(client);
    return;
  }
  http_coro_response_free(resp);

  http_client_stats_t stats;
  http_coro_client_get_stats(client, &stats);
  g_result.total_requests = stats.total_requests;
  g_result.successful_requests = stats.successful_requests;
  g_result.bytes_sent = stats.bytes_sent;
  g_result.bytes_received = stats.bytes_received;

  http_coro_client_destroy(client);
}

/* ── Specs ────────────────────────────────────────────────────────── */

spec("Coroutine HTTP Client Test") {

  it("should perform a GET request") {
    run_in_coro(test_coro_get);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check_int_eq(g_result.has_body, 1);
      check(g_result.body_len > 0);
    }
  }

  it("should POST JSON and receive a JSON response") {
    run_in_coro(test_coro_post_json);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check_int_eq(g_result.has_body, 1);
      check_int_eq(g_result.is_json, 1);
    }
  }

  it("should authenticate with a Bearer token") {
    run_in_coro(test_coro_bearer_auth);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check_int_eq(g_result.has_body, 1);
    }
  }

  it("should track request statistics") {
    run_in_coro(test_coro_stats);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check(g_result.total_requests == 1);
      check(g_result.successful_requests == 1);
      check(g_result.bytes_sent > 0);
      check(g_result.bytes_received > 0);
    }
  }
}
