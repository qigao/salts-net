#include "tinytest.h"
#include "http_client.h"
#include <string.h>

spec("http client basic") {

    describe("client lifecycle") {

        it("should create and destroy") {
            http_client_t *client = http_client_create();
            check_not_null(client);
            http_client_destroy(client);
        }

        it("should handle destroy NULL") {
            http_client_destroy(NULL);
            check(1);
        }
    }

    describe("client configuration") {
        static http_client_t *client;

        before_each() { client = http_client_create(); }
        after_each() { http_client_destroy(client); }

        it("should set timeout") {
            check_not_null(client);
            http_client_set_timeout(client, 5000);
            http_client_set_timeout(client, 0);
            http_client_set_timeout(client, -1);
            check(1);
        }

        it("should set user agent") {
            http_client_set_user_agent(client, "TestAgent/1.0");
            http_client_set_user_agent(client, "");
            http_client_set_user_agent(client, NULL);
            check(1);
        }

        it("should set follow redirects") {
            http_client_follow_redirects(client, 1);
            http_client_follow_redirects(client, 0);
            check(1);
        }

        it("should set max redirects") {
            http_client_set_max_redirects(client, 5);
            http_client_set_max_redirects(client, 0);
            http_client_set_max_redirects(client, 100);
            http_client_set_max_redirects(client, -1);
            check(1);
        }
    }

    describe("error handling") {

        it("should handle response free NULL") {
            http_response_free(NULL);
            check(1);
        }

        it("should return error for NULL url") {
            http_client_t *client = http_client_create();
            http_response_t *response = http_get(client, NULL);
            check_not_null(response);
            check_not_null(response->error);
            http_response_free(response);
            http_client_destroy(client);
        }

        it("should handle malformed url") {
            http_client_t *client = http_client_create();
            http_response_t *response = http_get(client, "not-a-url");
            check_not_null(response);
            http_response_free(response);
            http_client_destroy(client);
        }
    }
}
