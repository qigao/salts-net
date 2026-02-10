#include "tinytest.h"
#include "http_client.h"
#include <string.h>

static int request_called = 0;
static int response_called = 0;

static int test_request_interceptor(http_request_context_t *ctx) {
    request_called++;
    return 0;
}

static void test_response_interceptor(http_response_context_t *ctx) {
    response_called++;
}

static int abort_interceptor(http_request_context_t *ctx) {
    return 1;
}

static int user_data_value = 0;

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
            http_client_t *client = http_client_create();
            http_client_set_timeout(client, 3000);
            request_called = 0;
            response_called = 0;

            http_client_add_request_interceptor(client, test_request_interceptor, NULL);
            http_client_add_response_interceptor(client, test_response_interceptor, NULL);

            http_response_t *response = http_get(client, "https://httpbin.org/get");
            if (response->error) {
                http_response_free(response);
                http_client_destroy(client);
                return;
            }

            check_int_eq(request_called, 1);
            check_int_eq(response_called, 1);

            http_response_free(response);
            http_client_destroy(client);
        }
    }

    describe("abort") {

        it("should abort request via interceptor") {
            http_client_t *client = http_client_create();
            http_client_add_request_interceptor(client, abort_interceptor, NULL);

            http_response_t *response = http_get(client, "https://httpbin.org/get");
            check_not_null(response->error);
            check_str_contains(response->error, "aborted");

            http_response_free(response);
            http_client_destroy(client);
        }
    }

    describe("user data") {

        it("should pass user data to interceptors") {
            http_client_t *client = http_client_create();
            http_client_set_timeout(client, 3000);

            int req_data = 42;
            int resp_data = 99;
            http_client_add_request_interceptor(client, check_user_data_request, &req_data);
            http_client_add_response_interceptor(client, check_user_data_response, &resp_data);

            user_data_value = 0;
            http_response_t *response = http_get(client, "https://httpbin.org/get");
            if (response->error) {
                http_response_free(response);
                http_client_destroy(client);
                return;
            }

            check_int_eq(user_data_value, 99);

            http_response_free(response);
            http_client_destroy(client);
        }
    }

    describe("multiple") {

        it("should call all interceptors") {
            http_client_t *client = http_client_create();
            http_client_set_timeout(client, 3000);
            request_called = 0;
            response_called = 0;

            http_client_add_request_interceptor(client, test_request_interceptor, NULL);
            http_client_add_request_interceptor(client, test_request_interceptor, NULL);
            http_client_add_response_interceptor(client, test_response_interceptor, NULL);
            http_client_add_response_interceptor(client, test_response_interceptor, NULL);

            http_response_t *response = http_get(client, "https://httpbin.org/get");
            if (response->error) {
                http_response_free(response);
                http_client_destroy(client);
                return;
            }

            check_int_eq(request_called, 2);
            check_int_eq(response_called, 2);

            http_response_free(response);
            http_client_destroy(client);
        }
    }

    describe("clear") {

        it("should clear all interceptors") {
            http_client_t *client = http_client_create();
            http_client_set_timeout(client, 3000);
            request_called = 0;
            response_called = 0;

            http_client_add_request_interceptor(client, test_request_interceptor, NULL);
            http_client_add_response_interceptor(client, test_response_interceptor, NULL);
            http_client_clear_interceptors(client);

            http_response_t *response = http_get(client, "https://httpbin.org/get");
            if (response->error) {
                http_response_free(response);
                http_client_destroy(client);
                return;
            }

            check_int_eq(request_called, 0);
            check_int_eq(response_called, 0);

            http_response_free(response);
            http_client_destroy(client);
        }
    }
}
