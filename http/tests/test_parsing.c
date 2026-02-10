#include "tinytest.h"
#include "http_client.h"
#include <string.h>

spec("http response parsing") {

    describe("response structure") {

        it("should initialize to zero") {
            http_response_t *response = calloc(1, sizeof(http_response_t));
            check_not_null(response);
            check_int_eq(response->status_code, 0);
            check_null(response->headers);
            check_null(response->headers_list);
            check_null(response->body);
            check_null(response->error);
            check_size_eq(response->headers_len, 0);
            check_size_eq(response->body_len, 0);
            http_response_free(response);
        }

        it("should hold status code") {
            http_response_t *response = calloc(1, sizeof(http_response_t));
            response->status_code = 200;
            check_int_eq(response->status_code, 200);
            http_response_free(response);
        }

        it("should hold headers") {
            http_response_t *response = calloc(1, sizeof(http_response_t));
            const char *headers = "Content-Type: text/html\r\nContent-Length: 100\r\n";
            response->headers = strdup(headers);
            response->headers_len = strlen(headers);
            check_not_null(response->headers);
            check_int_gt((int)response->headers_len, 0);
            http_response_free(response);
        }

        it("should hold body") {
            http_response_t *response = calloc(1, sizeof(http_response_t));
            const char *body = "Hello, World!";
            response->body = strdup(body);
            response->body_len = strlen(body);
            check_not_null(response->body);
            check_size_eq(response->body_len, 13);
            check_str_eq(response->body, "Hello, World!");
            http_response_free(response);
        }

        it("should hold error") {
            http_response_t *response = calloc(1, sizeof(http_response_t));
            response->error = strdup("Connection failed");
            check_not_null(response->error);
            check_str_eq(response->error, "Connection failed");
            http_response_free(response);
        }

        it("should hold complete response") {
            http_response_t *response = calloc(1, sizeof(http_response_t));
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
            http_response_free(response);
        }
    }

    describe("URL parsing") {

        it("should parse http URL") {
            http_client_t *client = http_client_create();
            check_not_null(client);
            http_client_set_timeout(client, 1000);
            http_response_t *response = http_get(client, "http://localhost:9999/test");
            check_not_null(response);
            http_response_free(response);
            http_client_destroy(client);
        }
    }
}
