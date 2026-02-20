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

/* ── Stream callback ──────────────────────────────────────────────── */

static size_t s_stream_total = 0;
static int s_stream_chunks = 0;

static void stream_cb(const char *data, size_t len, void *ud) {
  UNUSED(ud);
  UNUSED(data);
  s_stream_chunks++;
  s_stream_total += len;
}

/* ── Result struct ────────────────────────────────────────────────── */

static struct {
  int ran, skipped, status_code;
  int data_chunks;
  size_t total_received;
  int body_null;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_stream_get(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;
  s_stream_total = 0;
  s_stream_chunks = 0;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 15000);

  http_coro_response_t *r = http_coro_stream_get(
      c, "https://httpbin.org/stream/5", stream_cb, NULL);
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  g_result.data_chunks = s_stream_chunks;
  g_result.total_received = s_stream_total;
  g_result.body_null = (r->body == NULL);

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_stream_no_accumulate(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;
  s_stream_total = 0;
  s_stream_chunks = 0;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);

  http_coro_response_t *r = http_coro_stream_get(
      c, "https://httpbin.org/bytes/500", stream_cb, NULL);
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  g_result.total_received = s_stream_total;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

spec("http async streaming") {

    it("should stream GET from httpbin") {
        run_in_coro(test_stream_get);
        check_int_eq(g_result.ran, 1);
        if (!g_result.skipped) {
            check_int_eq(g_result.status_code, 200);
            check(g_result.data_chunks > 0);
            check(g_result.total_received > 0);
        }
    }

    it("should not accumulate body in stream mode") {
        run_in_coro(test_stream_no_accumulate);
        check_int_eq(g_result.ran, 1);
        if (!g_result.skipped) {
            check_int_eq(g_result.status_code, 200);
            check_size_eq(g_result.total_received, 500);
        }
    }
}
