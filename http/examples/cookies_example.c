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
  int cookie_count;
  int has_session;
  int body_contains_session;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test function ───────────────────────────────────────────── */

static http_async_cookie_jar_t *g_jar = NULL;

static void test_auto_cookies(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);

  g_jar = http_async_cookie_jar_create();
  http_coro_client_set_cookie_jar(client, g_jar);

  http_coro_client_follow_redirects(client, 0);
  http_coro_response_t *response = http_coro_get(client, "https://httpbin.org/cookies/set?session=abc123");
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;
  g_result.cookie_count = http_async_cookie_jar_count(g_jar);

  const char *session = http_async_cookie_jar_get(g_jar, "session");
  g_result.has_session = (session != NULL && strcmp(session, "abc123") == 0);

  http_coro_response_free(response);

  /* Second request should send cookies automatically */
  http_coro_client_follow_redirects(client, 1);
  response = http_coro_get(client, "https://httpbin.org/cookies");
  if (!is_network_error(response)) {
    g_result.body_contains_session = (response->body && strstr(response->body, "\"session\": \"abc123\"") != NULL);
  }
  http_coro_response_free(response);

  http_coro_client_destroy(client);
}

spec("Cookie Management Test") {

  after() {
    if (g_jar) {
      http_async_cookie_jar_destroy(g_jar);
      g_jar = NULL;
    }
  }

  it("should successfully handle automatic cookies") {
    run_in_coro(test_auto_cookies);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 302);
      check(g_result.cookie_count >= 1);
      check_int_eq(g_result.has_session, 1);
      check_int_eq(g_result.body_contains_session, 1);
    }
  }

  it("should successfully perform manual cookie operations") {
    http_async_cookie_jar_t *jar = http_async_cookie_jar_create();

    http_async_cookie_jar_set(jar, "user_id", "12345");
    http_async_cookie_jar_set(jar, "preferences", "dark_mode");
    check(http_async_cookie_jar_count(jar) >= 2);
    check(strcmp(http_async_cookie_jar_get(jar, "user_id"), "12345") == 0);
    check(strcmp(http_async_cookie_jar_get(jar, "preferences"), "dark_mode") == 0);

    http_async_cookie_jar_remove(jar, "user_id");
    check(http_async_cookie_jar_get(jar, "user_id") == NULL);

    http_async_cookie_jar_clear(jar);
    check(http_async_cookie_jar_count(jar) == 0);

    http_async_cookie_jar_destroy(jar);
  }
}
