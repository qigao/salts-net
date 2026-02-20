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
  int is_json, is_html;
  int has_content_type, has_server;
  size_t content_length;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_json_headers(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);

  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/json");
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;

  char *content_type = http_coro_response_content_type(response);
  g_result.has_content_type = (content_type != NULL && strstr(content_type, "application/json") != NULL);
  free(content_type);

  g_result.content_length = http_coro_response_content_length(response);

  char *server = http_coro_response_get_header(response, "Server");
  g_result.has_server = (server != NULL);
  free(server);

  g_result.is_json = http_coro_response_is_json(response);
  g_result.is_html = http_coro_response_is_html(response);

  http_coro_response_free(response);
  http_coro_client_destroy(client);
}

static void test_html_headers(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);

  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/html");
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;
  g_result.is_json = http_coro_response_is_json(response);
  g_result.is_html = http_coro_response_is_html(response);

  http_coro_response_free(response);
  http_coro_client_destroy(client);
}

spec("Response Headers Test") {

  it("should successfully retrieve and check response headers for JSON") {
    run_in_coro(test_json_headers);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check_int_eq(g_result.has_content_type, 1);
      check(g_result.content_length > 0);
      check_int_eq(g_result.has_server, 1);
      check_int_eq(g_result.is_json, 1);
      check_int_eq(g_result.is_html, 0);
    }
  }

  it("should successfully retrieve and check response headers for HTML") {
    run_in_coro(test_html_headers);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check_int_eq(g_result.is_json, 0);
      check_int_eq(g_result.is_html, 1);
    }
  }
}
