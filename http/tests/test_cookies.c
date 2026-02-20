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
  int has_test_cookie;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static http_async_cookie_jar_t *g_jar = NULL;

static void test_auto_cookies(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);
  g_jar = http_async_cookie_jar_create();
  http_coro_client_set_cookie_jar(c, g_jar);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/cookies/set?test=value123");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }
  http_coro_response_free(r);

  const char *test_cookie = http_async_cookie_jar_get(g_jar, "test");
  g_result.has_test_cookie = (test_cookie != NULL && strcmp(test_cookie, "value123") == 0);

  http_coro_client_destroy(c);
}

static void test_no_jar(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/get");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }
  g_result.status_code = r->status_code;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

spec("http cookies") {

    describe("cookie jar") {

        it("should create and destroy") {
            http_async_cookie_jar_t *jar = http_async_cookie_jar_create();
            check_not_null(jar);
            check_size_eq(http_async_cookie_jar_count(jar), 0);
            http_async_cookie_jar_destroy(jar);
        }

        it("should set and get cookies") {
            http_async_cookie_jar_t *jar = http_async_cookie_jar_create();

            http_async_cookie_jar_set(jar, "session", "abc123");
            check_size_eq(http_async_cookie_jar_count(jar), 1);

            http_async_cookie_jar_set(jar, "user_id", "42");
            check_size_eq(http_async_cookie_jar_count(jar), 2);

            check_str_eq(http_async_cookie_jar_get(jar, "session"), "abc123");
            check_str_eq(http_async_cookie_jar_get(jar, "user_id"), "42");
            check_null(http_async_cookie_jar_get(jar, "nonexistent"));

            http_async_cookie_jar_destroy(jar);
        }

        it("should update existing cookie") {
            http_async_cookie_jar_t *jar = http_async_cookie_jar_create();
            http_async_cookie_jar_set(jar, "session", "abc123");
            http_async_cookie_jar_set(jar, "session", "xyz789");
            check_str_eq(http_async_cookie_jar_get(jar, "session"), "xyz789");
            check_size_eq(http_async_cookie_jar_count(jar), 1);
            http_async_cookie_jar_destroy(jar);
        }

        it("should remove cookies") {
            http_async_cookie_jar_t *jar = http_async_cookie_jar_create();
            http_async_cookie_jar_set(jar, "a", "1");
            http_async_cookie_jar_set(jar, "b", "2");
            http_async_cookie_jar_remove(jar, "a");
            check_size_eq(http_async_cookie_jar_count(jar), 1);
            check_null(http_async_cookie_jar_get(jar, "a"));
            check_not_null(http_async_cookie_jar_get(jar, "b"));
            http_async_cookie_jar_destroy(jar);
        }

        it("should clear all cookies") {
            http_async_cookie_jar_t *jar = http_async_cookie_jar_create();
            http_async_cookie_jar_set(jar, "a", "1");
            http_async_cookie_jar_set(jar, "b", "2");
            http_async_cookie_jar_set(jar, "c", "3");
            http_async_cookie_jar_clear(jar);
            check_size_eq(http_async_cookie_jar_count(jar), 0);
            http_async_cookie_jar_destroy(jar);
        }
    }

    describe("automatic handling") {

        it("should store cookies from Set-Cookie header") {
            run_in_coro(test_auto_cookies);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_int_eq(g_result.has_test_cookie, 1);
            }
            if (g_jar) { http_async_cookie_jar_destroy(g_jar); g_jar = NULL; }
        }

        it("should work without cookie jar") {
            run_in_coro(test_no_jar);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_int_eq(g_result.status_code, 200);
            }
        }
    }
}
