
#include "bdd-for-c.h"
#include "http_client.h"
#include <string.h>

spec("HTTP Headers & Auth Tests") {
    static http_client_t *client = NULL;

    before_each() {
        client = http_client_create();
        check(client != NULL);
    }

    after_each() {
        if (client) {
            http_client_destroy(client);
            client = NULL;
        }
    }

    it("should handle response headers helpers") {
        // Test with httpbin.org which returns predictable headers
        http_response_t *response = http_get(client, "https://httpbin.org/json");

        if (response->error) {
            // Skip test if network error
            http_response_free(response);
            return;
        }

        check(response->status_code == 200);

        // Test content type helper
        char *content_type = (char *)http_response_content_type(response);
        check(content_type != NULL);
        check(strstr(content_type, "application/json") != NULL);
        free(content_type);

        // Test content type checks
        check(http_response_is_json(response) == 1);
        check(http_response_is_html(response) == 0);

        // Test has_header
        check(http_response_has_header(response, "Content-Type") == 1);
        check(http_response_has_header(response, "NonExistent-Header") == 0);

        // Test get_header
        char *server = (char *)http_response_get_header(response, "Server");
        if (server) {
            check(strlen(server) > 0);
            free(server);
        }

        http_response_free(response);
    }

    it("should handle basic authentication") {
        http_client_set_basic_auth(client, "user", "passwd");

        http_response_t *response =
            http_get(client, "https://httpbin.org/basic-auth/user/passwd");

        if (response->error) {
            http_response_free(response);
            return;
        }

        check(response->status_code == 200);
        check(response->body != NULL);
        check(strstr(response->body, "authenticated") != NULL);

        http_response_free(response);
    }

    it("should reject wrong credentials") {
        http_client_set_basic_auth(client, "wrong", "credentials");
        http_response_t *response = http_get(client, "https://httpbin.org/basic-auth/user/passwd");

        if (!response->error) {
            check(response->status_code == 401);
        }

        http_response_free(response);
    }

    it("should handle bearer token authentication") {
        http_client_set_bearer_token(client, "my-secret-token");

        http_response_t *response = http_get(client, "https://httpbin.org/bearer");

        if (response->error) {
            http_response_free(response);
            return;
        }

        check(response->status_code == 200);
        check(response->body != NULL);
        check(strstr(response->body, "authenticated") != NULL);

        http_response_free(response);
    }

    it("should clear authentication correctly") {
        http_client_set_basic_auth(client, "user", "passwd");
        http_client_clear_auth(client);

        http_response_t *response = http_get(client, "https://httpbin.org/get");

        if (response->error) {
            http_response_free(response);
            return;
        }

        check(response->status_code == 200);
        http_response_free(response);
    }

    it("should return correct error codes") {
        // Test invalid URL
        http_response_t *response = http_get(client, "not-a-valid-url");
        check(response->error != NULL);
        check(response->error_code != HTTP_ERROR_NONE);
        http_response_free(response);

        // Test connection to non-existent host
        http_client_set_connect_timeout(client, 1000);
        response = http_get(client, "https://this-host-definitely-does-not-exist-12345.com");
        check(response->error != NULL);
        check(response->error_code != HTTP_ERROR_NONE);
        http_response_free(response);
    }
}
