#include "tinytest.h"
#include "http_client.h"
#include <string.h>

/* ── Network error check ─────────────────────────────────────────── */

static int is_network_error(http_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Interceptor callbacks ───────────────────────────────────────── */

static int request_called = 0;
static int response_called = 0;
static int user_data_value = 0;

static int test_request_interceptor(http_request_context_t *ctx) {
  (void)ctx;
  request_called++;
  return 0;
}

static void test_response_interceptor(http_response_context_t *ctx) {
  (void)ctx;
  response_called++;
}

static int abort_interceptor(http_request_context_t *ctx) {
  (void)ctx;
  return 1;
}

static int check_user_data_request(http_request_context_t *ctx) {
  user_data_value = *(int *)ctx->user_data;
  return 0;
}

static void check_user_data_response(http_response_context_t *ctx) {
  user_data_value = *(int *)ctx->user_data;
}

spec("http interceptors") {

    describe("basic") {
        it("should call request and response interceptors") {
            request_called = 0;
            response_called = 0;
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 10000);
            http_client_add_request_interceptor(c, test_request_interceptor, NULL);
            http_client_add_response_interceptor(c, test_response_interceptor, NULL);
            http_response_t *r = http_get(c, "https://httpbin.org/get");
            if (!is_network_error(r)) {
                check_int_eq(request_called, 1);
                check_int_eq(response_called, 1);
            }
            http_response_free(r);
            http_client_destroy(c);
        }
    }

    describe("abort") {
        it("should abort request via interceptor") {
            http_client_t *c = http_client_create();
            http_client_add_request_interceptor(c, abort_interceptor, NULL);
            http_response_t *r = http_get(c, "https://httpbin.org/get");
            check_not_null(r);
            check_not_null(r->error);
            check_str_contains(r->error, "aborted");
            http_response_free(r);
            http_client_destroy(c);
        }
    }

    describe("user data") {
        it("should pass user data to interceptors") {
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 10000);
            int req_data = 42;
            int resp_data = 99;
            http_client_add_request_interceptor(c, check_user_data_request, &req_data);
            http_client_add_response_interceptor(c, check_user_data_response, &resp_data);
            user_data_value = 0;
            http_response_t *r = http_get(c, "https://httpbin.org/get");
            if (!is_network_error(r)) {
                check_int_eq(user_data_value, 99);
            }
            http_response_free(r);
            http_client_destroy(c);
        }
    }

    describe("multiple") {
        it("should call all interceptors") {
            request_called = 0;
            response_called = 0;
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 10000);
            http_client_add_request_interceptor(c, test_request_interceptor, NULL);
            http_client_add_request_interceptor(c, test_request_interceptor, NULL);
            http_client_add_response_interceptor(c, test_response_interceptor, NULL);
            http_client_add_response_interceptor(c, test_response_interceptor, NULL);
            http_response_t *r = http_get(c, "https://httpbin.org/get");
            if (!is_network_error(r)) {
                check_int_eq(request_called, 2);
                check_int_eq(response_called, 2);
            }
            http_response_free(r);
            http_client_destroy(c);
        }
    }

    describe("clear") {
        it("should clear all interceptors") {
            request_called = 0;
            response_called = 0;
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 10000);
            http_client_add_request_interceptor(c, test_request_interceptor, NULL);
            http_client_add_response_interceptor(c, test_response_interceptor, NULL);
            http_client_clear_interceptors(c);
            http_response_t *r = http_get(c, "https://httpbin.org/get");
            if (!is_network_error(r)) {
                check_int_eq(request_called, 0);
                check_int_eq(response_called, 0);
            }
            http_response_free(r);
            http_client_destroy(c);
        }
    }
}
