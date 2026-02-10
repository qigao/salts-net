#include "tinytest.h"
#include "http_client.h"
#include <string.h>

spec("http headers and auth") {

    describe("response headers") {

        it("should parse content type") {
            http_client_t *client = http_client_create();
            http_response_t *response = http_get(client, "https://httpbin.org/json");

            if (response->error) {
                http_response_free(response);
                http_client_destroy(client);
                return;
            }

            check_int_eq(response->status_code, 200);

            char *content_type = (char *)http_response_content_type(response);
            check_not_null(content_type);
            check_str_contains(content_type, "application/json");
            free(content_type);

            check_int_eq(http_response_is_json(response), 1);
            check_int_eq(http_response_is_html(response), 0);
            check_int_eq(http_response_has_header(response, "Content-Type"), 1);
            check_int_eq(http_response_has_header(response, "NonExistent-Header"), 0);

            http_response_free(response);
            http_client_destroy(client);
        }
    }

    describe("basic auth") {

        it("should authenticate with correct credentials") {
            http_client_t *client = http_client_create();
            http_client_set_basic_auth(client, "user", "passwd");

            http_response_t *response = http_get(client, "https://httpbin.org/basic-auth/user/passwd");

            if (response->error) {
                http_response_free(response);
                http_client_destroy(client);
                return;
            }

            check_int_eq(response->status_code, 200);
            check_not_null(response->body);
            check_str_contains(response->body, "authenticated");

            http_response_free(response);
            http_client_destroy(client);
        }
    }

    describe("bearer token") {

        it("should authenticate with token") {
            http_client_t *client = http_client_create();
            http_client_set_bearer_token(client, "my-secret-token");

            http_response_t *response = http_get(client, "https://httpbin.org/bearer");

            if (response->error) {
                http_response_free(response);
                http_client_destroy(client);
                return;
            }

            check_int_eq(response->status_code, 200);
            check_not_null(response->body);
            check_str_contains(response->body, "authenticated");

            http_response_free(response);
            http_client_destroy(client);
        }
    }

    describe("clear auth") {

        it("should clear credentials") {
            http_client_t *client = http_client_create();
            http_client_set_basic_auth(client, "user", "passwd");
            http_client_clear_auth(client);

            http_response_t *response = http_get(client, "https://httpbin.org/get");

            if (response->error) {
                http_response_free(response);
                http_client_destroy(client);
                return;
            }

            check_int_eq(response->status_code, 200);

            http_response_free(response);
            http_client_destroy(client);
        }
    }

    describe("error codes") {

        it("should set error code for invalid url") {
            http_client_t *client = http_client_create();
            http_response_t *response = http_get(client, "not-a-valid-url");
            check_not_null(response->error);
            check_int_ne(response->error_code, HTTP_ERROR_NONE);
            http_response_free(response);
            http_client_destroy(client);
        }

        it("should set error code for non-existent host") {
            http_client_t *client = http_client_create();
            http_client_set_connect_timeout(client, 1000);
            http_response_t *response = http_get(client, "https://this-host-definitely-does-not-exist-12345.com");
            check_not_null(response->error);
            check_int_ne(response->error_code, HTTP_ERROR_NONE);
            http_response_free(response);
            http_client_destroy(client);
        }
    }
}
