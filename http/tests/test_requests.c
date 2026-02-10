#include "tinytest.h"
#include "http_client.h"
#include <string.h>

spec("http requests") {

    static http_client_t *client;

    before_each() { client = http_client_create(); }
    after_each() { http_client_destroy(client); client = NULL; }

    describe("GET") {

        it("should return error for NULL url") {
            http_response_t *response = http_get(client, NULL);
            check_not_null(response);
            check_not_null(response->error);
            http_response_free(response);
        }
    }

    describe("POST") {

        it("should return error for NULL url with body") {
            const char *body = "{\"test\":\"data\"}";
            http_response_t *response = http_post(client, NULL, body, strlen(body));
            check_not_null(response);
            check_not_null(response->error);
            http_response_free(response);
        }

        it("should return error for NULL url with empty body") {
            http_response_t *response = http_post(client, NULL, NULL, 0);
            check_not_null(response);
            check_not_null(response->error);
            http_response_free(response);
        }
    }

    describe("custom headers") {

        it("should accept custom headers") {
            const char *headers[] = {
                "X-Custom-Header: value1",
                "X-Another-Header: value2"
            };
            http_response_t *response = http_request(client, HTTP_GET, NULL, headers, 2, NULL, 0);
            check_not_null(response);
            check_not_null(response->error);
            http_response_free(response);
        }

        it("should accept body and headers together") {
            const char *headers[] = {"Content-Type: application/json"};
            const char *body = "{\"key\":\"value\"}";
            http_response_t *response = http_request(client, HTTP_POST, NULL, headers, 1, body, strlen(body));
            check_not_null(response);
            check_not_null(response->error);
            http_response_free(response);
        }
    }

    describe("HTTP methods") {

        it("should accept all standard methods") {
            http_method_t methods[] = {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE, HTTP_HEAD, HTTP_PATCH};
            for (int i = 0; i < 6; i++) {
                http_response_t *response = http_request(client, methods[i], NULL, NULL, 0, NULL, 0);
                check_not_null(response);
                check_not_null(response->error);
                http_response_free(response);
            }
        }
    }

    describe("URL parsing") {

        it("should handle query params") {
            http_client_set_timeout(client, 100);
            http_response_t *response = http_get(client, "http://127.0.0.1:1/test?param1=value1&param2=value2");
            check_not_null(response);
            http_response_free(response);
        }

        it("should handle fragment") {
            http_client_set_timeout(client, 100);
            http_response_t *response = http_get(client, "http://127.0.0.1:1/test#fragment");
            check_not_null(response);
            http_response_free(response);
        }
    }
}
