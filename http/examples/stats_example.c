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
  int ran, skipped;
  uint64_t total_requests, successful_requests, bytes_received, redirects;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_track_stats(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);
  http_coro_client_reset_stats(client);

  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/get");
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    return;
  }
  http_coro_response_free(response);

  http_async_client_stats_t stats;
  http_coro_client_get_stats(client, &stats);
  g_result.total_requests = stats.total_requests;
  g_result.successful_requests = stats.successful_requests;
  g_result.bytes_received = stats.bytes_received;

  http_coro_client_destroy(client);
}

static void test_track_redirects(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);
  http_coro_client_reset_stats(client);

  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/redirect/1");
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    return;
  }
  http_coro_response_free(response);

  http_async_client_stats_t stats;
  http_coro_client_get_stats(client, &stats);
  g_result.redirects = stats.redirects_followed;

  http_coro_client_destroy(client);
}

static void test_reset_stats(turbo_coro_context_t *ctx) {
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
  http_coro_response_free(response);

  http_coro_client_reset_stats(client);

  http_async_client_stats_t stats;
  http_coro_client_get_stats(client, &stats);
  g_result.total_requests = stats.total_requests;

  http_coro_client_destroy(client);
}

spec("Client Statistics Test") {

  it("should successfully track client statistics") {
    run_in_coro(test_track_stats);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check(g_result.total_requests == 1);
      check(g_result.successful_requests == 1);
      check(g_result.bytes_received > 0);
    }
  }

  it("should successfully track redirects") {
    run_in_coro(test_track_redirects);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check(g_result.redirects == 1);
    }
  }

  it("should successfully reset statistics") {
    run_in_coro(test_reset_stats);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check(g_result.total_requests == 0);
    }
  }
}
