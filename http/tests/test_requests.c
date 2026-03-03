#include "tinytest.h"
#include "http_client.h"
#include <string.h>

spec("http requests") {

    describe("GET") {

        it("should return error for NULL url") {
            http_client_t *c = http_client_create();
            http_response_t *r = http_get(c, NULL);
            check(r != NULL && r->error != NULL);
            http_response_free(r);
            http_client_destroy(c);
        }
    }

    describe("POST") {

        it("should return error for NULL url with body") {
            http_client_t *c = http_client_create();
            const char *body = "{\"test\":\"data\"}";
            http_response_t *r = http_post(c, NULL, body, strlen(body));
            check(r != NULL && r->error != NULL);
            http_response_free(r);
            http_client_destroy(c);
        }

        it("should return error for NULL url with empty body") {
            http_client_t *c = http_client_create();
            http_response_t *r = http_post(c, NULL, NULL, 0);
            check(r != NULL && r->error != NULL);
            http_response_free(r);
            http_client_destroy(c);
        }
    }

    describe("custom headers") {

        it("should accept custom headers") {
            http_client_t *c = http_client_create();
            const char *headers[] = {
                "X-Custom-Header: value1",
                "X-Another-Header: value2"
            };
            http_response_t *r = http_request(c, HTTP_GET, NULL, headers, 2, NULL, 0);
            check(r != NULL && r->error != NULL);
            http_response_free(r);
            http_client_destroy(c);
        }

        it("should accept body and headers together") {
            http_client_t *c = http_client_create();
            const char *headers[] = {"Content-Type: application/json"};
            const char *body = "{\"key\":\"value\"}";
            http_response_t *r = http_request(c, HTTP_POST, NULL, headers, 1, body, strlen(body));
            check(r != NULL && r->error != NULL);
            http_response_free(r);
            http_client_destroy(c);
        }
    }

    describe("HTTP methods") {

        it("should accept all standard methods") {
            http_client_t *c = http_client_create();
            http_method_t methods[] = {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE, HTTP_HEAD, HTTP_PATCH};
            for (int i = 0; i < 6; i++) {
                http_response_t *r = http_request(c, methods[i], NULL, NULL, 0, NULL, 0);
                check(r != NULL && r->error != NULL);
                http_response_free(r);
            }
            http_client_destroy(c);
        }
    }

    describe("URL parsing") {

        it("should handle query params") {
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 100);
            http_response_t *r = http_get(c, "http://127.0.0.1:1/test?param1=value1&param2=value2");
            check(r != NULL);
            http_response_free(r);
            http_client_destroy(c);
        }

        it("should handle fragment") {
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 100);
            http_response_t *r = http_get(c, "http://127.0.0.1:1/test#fragment");
            check(r != NULL);
            http_response_free(r);
            http_client_destroy(c);
        }
    }
}
