#include "tinytest.h"
#include "http_coro_client.h"
#include <turbo_coro.h>
#include <string.h>
#include <stdio.h>

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

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Concurrent fetch state ──────────────────────────────────────── */

#define MAX_URLS 8

typedef struct {
  turbo_coro_context_t *ctx;
  const char **urls;
  int url_count;
  int status_codes[MAX_URLS];
  size_t body_lens[MAX_URLS];
  int completed;
  int succeeded;
} concurrent_ctx_t;

static void fetch_worker(turbo_coro_t *co, void *arg) {
  UNUSED(co);
  concurrent_ctx_t *ctx = (concurrent_ctx_t *)arg;

  http_coro_client_t *client = http_coro_client_create(ctx->ctx);
  http_coro_client_set_timeout(client, 10000);
  http_coro_client_set_user_agent(client, "TurboNet-CoroConcurrent/1.0");

  while (1) {
    int idx = ctx->completed++;
    if (idx >= ctx->url_count) break;

    http_coro_response_t *resp = http_coro_get(client, ctx->urls[idx]);
    if (resp) {
      ctx->status_codes[idx] = resp->status_code;
      ctx->body_lens[idx] = resp->body_len;
      if (resp->error_code == HTTP_ERROR_NONE)
        ctx->succeeded++;
      http_coro_response_free(resp);
    }
  }

  http_coro_client_destroy(client);
}

/* ── Result struct ────────────────────────────────────────────────── */

static struct {
  int ran, skipped;
  int total, succeeded;
  int all_status_ok;
  int all_have_body;
} g_result;

/* ── Coro test function ──────────────────────────────────────────── */

static void test_concurrent_fetch(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  const char *urls[] = {
    "https://httpbin.org/get",
    "https://httpbin.org/ip",
    "https://httpbin.org/user-agent",
    "https://httpbin.org/headers",
  };
  int num_urls = sizeof(urls) / sizeof(urls[0]);

  /* Quick connectivity check */
  http_coro_client_t *probe = http_coro_client_create(ctx);
  http_coro_client_set_timeout(probe, 10000);
  http_coro_response_t *resp = http_coro_get(probe, urls[0]);
  if (is_network_error(resp)) {
    g_result.skipped = 1;
    http_coro_response_free(resp);
    http_coro_client_destroy(probe);
    return;
  }
  http_coro_response_free(resp);
  http_coro_client_destroy(probe);

  /* Spawn 2 worker coroutines to fetch 4 URLs concurrently */
  concurrent_ctx_t cctx;
  memset(&cctx, 0, sizeof(cctx));
  cctx.ctx = ctx;
  cctx.urls = urls;
  cctx.url_count = num_urls;

  int num_workers = 2;
  turbo_coro_scheduler_t *sched = turbo_coro_current_scheduler();
  for (int i = 0; i < num_workers; i++) {
    turbo_coro_spawn(sched, fetch_worker, &cctx);
  }

  /* Yield until workers finish (scheduler drives them) */
  while (turbo_coro_scheduler_count(sched) > 1) {
    turbo_coro_yield();
  }

  g_result.total = num_urls;
  g_result.succeeded = cctx.succeeded;
  g_result.all_status_ok = 1;
  g_result.all_have_body = 1;
  for (int i = 0; i < num_urls; i++) {
    if (cctx.status_codes[i] != 200) g_result.all_status_ok = 0;
    if (cctx.body_lens[i] == 0) g_result.all_have_body = 0;
  }
}

/* ── Specs ────────────────────────────────────────────────────────── */

spec("Concurrent Coroutine HTTP Fetch Test") {

  it("should fetch multiple URLs concurrently using coroutine workers") {
    run_in_coro(test_concurrent_fetch);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.succeeded, g_result.total);
      check_int_eq(g_result.all_status_ok, 1);
      check_int_eq(g_result.all_have_body, 1);
    }
  }
}
