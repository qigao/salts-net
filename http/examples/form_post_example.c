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
  int body_contains_name;
  int body_contains_search;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_form_post(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);

  http_async_params_t *params = http_async_params_create();
  http_async_params_add(params, "name", "John Doe");
  http_async_params_add(params, "message", "Hello from HTTP client!");

  http_coro_response_t *response = http_coro_post_form(client, "https://httpbin.org/post", params);
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_async_params_free(params);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;
  g_result.body_contains_name = (response->body && strstr(response->body, "John Doe") != NULL);

  http_coro_response_free(response);
  http_async_params_free(params);
  http_coro_client_destroy(client);
}

static void test_build_url(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);

  http_async_params_t *query = http_async_params_create();
  http_async_params_add(query, "search", "http client");
  http_async_params_add(query, "page", "1");

  char *url = http_async_build_url("https://httpbin.org/get", query);
  g_result.body_contains_search = (url != NULL && strstr(url, "search=http+client") != NULL);

  http_coro_response_t *response = http_coro_get(client, url);
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_async_params_free(query);
    free(url);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;

  http_coro_response_free(response);
  http_async_params_free(query);
  free(url);
  http_coro_client_destroy(client);
}

spec("Form Post Test") {

  it("should successfully perform a URL-encoded form POST") {
    run_in_coro(test_form_post);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check_int_eq(g_result.body_contains_name, 1);
    }
  }

  it("should successfully build URLs with query parameters") {
    run_in_coro(test_build_url);
    check_int_eq(g_result.ran, 1);
    check_int_eq(g_result.body_contains_search, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
    }
  }
}
