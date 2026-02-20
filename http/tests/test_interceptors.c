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

/* ── Interceptor callbacks ────────────────────────────────────────── */

static int request_called = 0;
static int response_called = 0;
static int user_data_value = 0;

static int test_request_interceptor(http_async_request_context_t *ctx) {
  (void)ctx;
  request_called++;
  return 0;
}

static void test_response_interceptor(http_async_response_context_t *ctx) {
  (void)ctx;
  response_called++;
}

static int abort_interceptor(http_async_request_context_t *ctx) {
  (void)ctx;
  return 1;
}

static int check_user_data_request(http_async_request_context_t *ctx) {
  user_data_value = *(int *)ctx->user_data;
  return 0;
}

static void check_user_data_response(http_async_response_context_t *ctx) {
  user_data_value = *(int *)ctx->user_data;
}

/* ── Result struct ────────────────────────────────────────────────── */

static struct {
  int ran, skipped;
  int req_called, resp_called;
  int has_error;
  int user_data_val;
  char error_msg[256];
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_basic_interceptors(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;
  request_called = 0;
  response_called = 0;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);
  http_coro_client_add_request_interceptor(c, test_request_interceptor, NULL);
  http_coro_client_add_response_interceptor(c, test_response_interceptor, NULL);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/get");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.req_called = request_called;
  g_result.resp_called = response_called;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_abort(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_add_request_interceptor(c, abort_interceptor, NULL);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/get");
  g_result.has_error = (r->error != NULL);
  if (r->error && strlen(r->error) < sizeof(g_result.error_msg))
    strncpy(g_result.error_msg, r->error, sizeof(g_result.error_msg) - 1);

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_user_data(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);

  int req_data = 42;
  int resp_data = 99;
  http_coro_client_add_request_interceptor(c, check_user_data_request, &req_data);
  http_coro_client_add_response_interceptor(c, check_user_data_response, &resp_data);

  user_data_value = 0;
  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/get");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.user_data_val = user_data_value;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_multiple(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;
  request_called = 0;
  response_called = 0;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);
  http_coro_client_add_request_interceptor(c, test_request_interceptor, NULL);
  http_coro_client_add_request_interceptor(c, test_request_interceptor, NULL);
  http_coro_client_add_response_interceptor(c, test_response_interceptor, NULL);
  http_coro_client_add_response_interceptor(c, test_response_interceptor, NULL);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/get");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.req_called = request_called;
  g_result.resp_called = response_called;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_clear(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;
  request_called = 0;
  response_called = 0;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);
  http_coro_client_add_request_interceptor(c, test_request_interceptor, NULL);
  http_coro_client_add_response_interceptor(c, test_response_interceptor, NULL);
  http_coro_client_clear_interceptors(c);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/get");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.req_called = request_called;
  g_result.resp_called = response_called;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

spec("http interceptors") {

    describe("basic") {
        it("should call request and response interceptors") {
            run_in_coro(test_basic_interceptors);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_int_eq(g_result.req_called, 1);
                check_int_eq(g_result.resp_called, 1);
            }
        }
    }

    describe("abort") {
        it("should abort request via interceptor") {
            run_in_coro(test_abort);
            check_int_eq(g_result.ran, 1);
            check_int_eq(g_result.has_error, 1);
            check_str_contains(g_result.error_msg, "aborted");
        }
    }

    describe("user data") {
        it("should pass user data to interceptors") {
            run_in_coro(test_user_data);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_int_eq(g_result.user_data_val, 99);
            }
        }
    }

    describe("multiple") {
        it("should call all interceptors") {
            run_in_coro(test_multiple);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_int_eq(g_result.req_called, 2);
                check_int_eq(g_result.resp_called, 2);
            }
        }
    }

    describe("clear") {
        it("should clear all interceptors") {
            run_in_coro(test_clear);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_int_eq(g_result.req_called, 0);
                check_int_eq(g_result.resp_called, 0);
            }
        }
    }
}
