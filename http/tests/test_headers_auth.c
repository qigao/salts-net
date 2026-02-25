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
  int has_content_type, is_json, is_html;
  int has_header_ct, no_header_missing;
  int has_body, body_has_auth;
  int error_code, has_error;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_content_type(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);
  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/json");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  char *ct = http_coro_response_content_type(r);
  g_result.has_content_type = (ct != NULL && strstr(ct, "application/json") != NULL);
  free(ct);
  g_result.is_json = http_coro_response_is_json(r);
  g_result.is_html = http_coro_response_is_html(r);
  g_result.has_header_ct = http_coro_response_has_header(r, "Content-Type");
  g_result.no_header_missing = !http_coro_response_has_header(r, "NonExistent-Header");

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_basic_auth(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);
  http_coro_client_set_basic_auth(c, "user", "passwd");
  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/basic-auth/user/passwd");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  g_result.has_body = (r->body != NULL);
  g_result.body_has_auth = (r->body && strstr(r->body, "authenticated") != NULL);

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_bearer_token(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);
  http_coro_client_set_bearer_token(c, "my-secret-token");
  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/bearer");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  g_result.has_body = (r->body != NULL);
  g_result.body_has_auth = (r->body && strstr(r->body, "authenticated") != NULL);

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_clear_auth(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);
  http_coro_client_set_basic_auth(c, "user", "passwd");
  http_coro_client_clear_auth(c);
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

static void test_invalid_url(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_response_t *r = http_coro_get(c, "not-a-valid-url");
  g_result.has_error = (r->error != NULL);
  g_result.error_code = r->error_code;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_nonexistent_host(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_connect_timeout(c, 1000);
  http_coro_response_t *r = http_coro_get(c, "https://this-host-definitely-does-not-exist-12345.com");
  g_result.has_error = (r->error != NULL);
  g_result.error_code = r->error_code;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

spec("http headers and auth") {

    describe("response headers") {

        it("should parse content type") {
            run_in_coro(test_content_type);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_int_eq(g_result.status_code, 200);
                check_int_eq(g_result.has_content_type, 1);
                check_int_eq(g_result.is_json, 1);
                check_int_eq(g_result.is_html, 0);
                check_int_eq(g_result.has_header_ct, 1);
                check_int_eq(g_result.no_header_missing, 1);
            }
        }
    }

    describe("basic auth") {

        it("should authenticate with correct credentials") {
            run_in_coro(test_basic_auth);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_int_eq(g_result.status_code, 200);
                check_int_eq(g_result.has_body, 1);
                check_int_eq(g_result.body_has_auth, 1);
            }
        }
    }

    describe("bearer token") {

        it("should authenticate with token") {
            run_in_coro(test_bearer_token);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_int_eq(g_result.status_code, 200);
                check_int_eq(g_result.has_body, 1);
                check_int_eq(g_result.body_has_auth, 1);
            }
        }
    }

    describe("clear auth") {

        it("should clear credentials") {
            run_in_coro(test_clear_auth);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_int_eq(g_result.status_code, 200);
            }
        }
    }

    describe("error codes") {

        it("should set error code for invalid url") {
            run_in_coro(test_invalid_url);
            check_int_eq(g_result.ran, 1);
            check_int_eq(g_result.has_error, 1);
            check_int_ne(g_result.error_code, HTTP_ERROR_NONE);
        }

        it("should set error code for non-existent host") {
            run_in_coro(test_nonexistent_host);
            check_int_eq(g_result.ran, 1);
            check_int_eq(g_result.has_error, 1);
            check_int_ne(g_result.error_code, HTTP_ERROR_NONE);
        }
    }
}
