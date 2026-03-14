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

spec("http stats and forms") {

    describe("statistics") {

        it("should start at zero") {
            http_client_t *client = http_client_create("http://localhost:8080");
            http_client_stats_t stats;
            http_client_get_stats(client, &stats);
            check_size_eq(stats.total_requests, 0);
            check_size_eq(stats.successful_requests, 0);
            http_client_destroy(client);
        }

        it("should track requests") {
            http_client_t *c = http_client_create("http://localhost:8080");
            http_client_set_timeout(c, 10000);
            http_response_t *r = http_get(c, "https://httpbin.org/get");
            if (!is_network_error(r)) {
                http_response_free(r);
                http_client_stats_t stats;
                http_client_get_stats(c, &stats);
                check_size_eq(stats.total_requests, 1);
                check_size_eq(stats.successful_requests, 1);
                http_client_reset_stats(c);
                http_client_get_stats(c, &stats);
                check_size_eq(stats.total_requests, 0);
            } else {
                http_response_free(r);
            }
            http_client_destroy(c);
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
            http_client_t *c = http_client_create("http://localhost:8080");
            http_client_set_timeout(c, 10000);
            http_params_t *params = http_params_create();
            http_params_add(params, "name", "Test User");
            http_params_add(params, "value", "123");
            http_response_t *r = http_post_form(c, "https://httpbin.org/post", params);
            if (!is_network_error(r)) {
                check_int_eq(r->status_code, 200);
                check(r->body && strstr(r->body, "application/x-www-form-urlencoded") != NULL);
            }
            http_response_free(r);
            http_params_free(params);
            http_client_destroy(c);
        }
    }
}
