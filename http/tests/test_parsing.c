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

/* ── Coro test for URL parsing ────────────────────────────────────── */

static int g_parse_ran = 0;

static int g_parse_result = 0;

static void test_parse_http_url(turbo_coro_context_t *ctx) {
  g_parse_ran = 1;
  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 1000);
  http_coro_response_t *r = http_coro_get(c, "http://localhost:9999/test");
  
  if (r) {
      g_parse_result = 1;
      http_coro_response_free(r);
  } else {
      g_parse_result = 0;
  }
  
  http_coro_client_destroy(c);
}

spec("http response parsing") {

    describe("response structure") {

        it("should initialize to zero") {
            http_coro_response_t *response = calloc(1, sizeof(http_coro_response_t));
            check_not_null(response);
            check_int_eq(response->status_code, 0);
            check_null(response->headers);
            check_null(response->body);
            check_null(response->error);
            check_size_eq(response->headers_len, 0);
            check_size_eq(response->body_len, 0);
            http_coro_response_free(response);
        }

        it("should hold status code") {
            http_coro_response_t *response = calloc(1, sizeof(http_coro_response_t));
            response->status_code = 200;
            check_int_eq(response->status_code, 200);
            http_coro_response_free(response);
        }

        it("should hold headers") {
            http_coro_response_t *response = calloc(1, sizeof(http_coro_response_t));
            const char *headers = "Content-Type: text/html\r\nContent-Length: 100\r\n";
            response->headers = strdup(headers);
            response->headers_len = strlen(headers);
            check_not_null(response->headers);
            check_int_gt((int)response->headers_len, 0);
            http_coro_response_free(response);
        }

        it("should hold body") {
            http_coro_response_t *response = calloc(1, sizeof(http_coro_response_t));
            const char *body = "Hello, World!";
            response->body = strdup(body);
            response->body_len = strlen(body);
            check_not_null(response->body);
            check_size_eq(response->body_len, 13);
            check_str_eq(response->body, "Hello, World!");
            http_coro_response_free(response);
        }

        it("should hold error") {
            http_coro_response_t *response = calloc(1, sizeof(http_coro_response_t));
            response->error = strdup("Connection failed");
            check_not_null(response->error);
            check_str_eq(response->error, "Connection failed");
            http_coro_response_free(response);
        }

        it("should hold complete response") {
            http_coro_response_t *response = calloc(1, sizeof(http_coro_response_t));
            response->status_code = 200;
            response->headers = strdup("Content-Type: application/json\r\n");
            response->headers_len = strlen(response->headers);
            response->body = strdup("{\"status\":\"ok\"}");
            response->body_len = strlen(response->body);
            check_int_eq(response->status_code, 200);
            check_not_null(response->headers);
            check_not_null(response->body);
            check_int_gt((int)response->headers_len, 0);
            check_int_gt((int)response->body_len, 0);
            http_coro_response_free(response);
        }
    }

    describe("URL parsing") {

        it("should parse http URL") {
            g_parse_ran = 0;
            g_parse_result = 0;
            run_in_coro(test_parse_http_url);
            check_int_eq(g_parse_ran, 1);
            check_int_eq(g_parse_result, 1);
        }
    }
}
