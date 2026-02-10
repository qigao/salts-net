#include "tinytest.h"
#include "http_client.h"
#include <string.h>

spec("http stats and forms") {

    describe("statistics") {

        it("should start at zero") {
            http_client_t *client = http_client_create();
            http_client_stats_t stats;
            http_client_get_stats(client, &stats);
            check_size_eq(stats.total_requests, 0);
            check_size_eq(stats.successful_requests, 0);
            http_client_destroy(client);
        }

        it("should track requests") {
            http_client_t *client = http_client_create();
            http_response_t *response = http_get(client, "https://httpbin.org/get");

            if (response->error) {
                http_response_free(response);
                http_client_destroy(client);
                return;
            }

            http_response_free(response);

            http_client_stats_t stats;
            http_client_get_stats(client, &stats);
            check_size_eq(stats.total_requests, 1);
            check_size_eq(stats.successful_requests, 1);

            http_client_reset_stats(client);
            http_client_get_stats(client, &stats);
            check_size_eq(stats.total_requests, 0);

            http_client_destroy(client);
        }
    }

    describe("URL params") {

        it("should encode params") {
            http_params_t *params = http_params_create();
            check_not_null(params);

            http_params_add(params, "name", "John Doe");
            http_params_add(params, "email", "test@example.com");

            char *encoded = http_params_encode(params);
            check_not_null(encoded);
            check(strlen(encoded) > 0, "encoded string should not be empty");
            check(strstr(encoded, "name=John+Doe") != NULL ||
                  strstr(encoded, "name=John%20Doe") != NULL,
                  "name should be encoded");
            check_str_contains(encoded, "email=test%40example.com");

            free(encoded);
            http_params_free(params);
        }

        it("should build URL with params") {
            http_params_t *params = http_params_create();
            http_params_add(params, "page", "1");
            http_params_add(params, "limit", "10");

            char *url = http_build_url("https://example.com/api", params);
            check_not_null(url);
            check_str_contains(url, "https://example.com/api?");
            check_str_contains(url, "page=1");
            check_str_contains(url, "limit=10");
            free(url);

            url = http_build_url("https://example.com/api?existing=value", params);
            check_not_null(url);
            check_str_contains(url, "existing=value");
            free(url);

            http_params_free(params);
        }
    }

    describe("form POST") {

        it("should post form data") {
            http_client_t *client = http_client_create();
            http_params_t *params = http_params_create();
            http_params_add(params, "name", "Test User");
            http_params_add(params, "value", "123");

            http_response_t *response = http_post_form(client, "https://httpbin.org/post", params);

            if (response->error) {
                http_response_free(response);
                http_params_free(params);
                http_client_destroy(client);
                return;
            }

            check_int_eq(response->status_code, 200);
            if (response->body)
                check_str_contains(response->body, "application/x-www-form-urlencoded");

            http_response_free(response);
            http_params_free(params);
            http_client_destroy(client);
        }
    }
}
