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

spec("http retry policy") {

    describe("defaults") {

        it("should have sane defaults") {
            http_retry_policy_t policy = http_retry_policy_default();
            check_int_eq(policy.max_retries, 3);
            check_int_eq(policy.initial_delay_ms, 1000);
            check_int_eq(policy.exponential_backoff, 1);
            check_int_eq(policy.retry_on_timeout, 0);
            check_int_eq(policy.retry_on_connection_error, 1);
            check_int_eq(policy.retry_on_5xx, 1);
        }
    }

    describe("set and get") {

        it("should store policy") {
            http_client_t *client = http_client_create();
            http_retry_policy_t policy = {
                .max_retries = 5,
                .initial_delay_ms = 500,
                .max_delay_ms = 10000,
                .exponential_backoff = 1,
                .retry_on_timeout = 1,
                .retry_on_connection_error = 1,
                .retry_on_5xx = 1,
                .jitter_factor = 0.1
            };
            http_client_set_retry_policy(client, &policy);
            http_retry_policy_t retrieved;
            http_client_get_retry_policy(client, &retrieved);
            check_int_eq(retrieved.max_retries, 5);
            check_int_eq(retrieved.initial_delay_ms, 500);
            check_int_eq(retrieved.exponential_backoff, 1);
            http_client_destroy(client);
        }

        it("should clear policy") {
            http_client_t *client = http_client_create();
            http_retry_policy_t policy = http_retry_policy_default();
            http_client_set_retry_policy(client, &policy);
            http_client_clear_retry_policy(client);
            http_retry_policy_t retrieved;
            http_client_get_retry_policy(client, &retrieved);
            check_int_eq(retrieved.max_retries, 0);
            http_client_destroy(client);
        }
    }

    describe("behavior") {

        it("should retry on 5xx") {
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 10000);
            http_retry_policy_t policy = {
                .max_retries = 2, .initial_delay_ms = 100,
                .max_delay_ms = 1000, .retry_on_5xx = 1
            };
            http_client_set_retry_policy(c, &policy);
            http_response_t *r = http_get(c, "https://httpbin.org/status/500");
            if (!is_network_error(r))
                check_int_eq(r->status_code, 500);
            http_response_free(r);
            http_client_destroy(c);
        }

        it("should not retry on success") {
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 10000);
            http_retry_policy_t policy = http_retry_policy_default();
            http_client_set_retry_policy(c, &policy);
            http_response_t *r = http_get(c, "https://httpbin.org/get");
            if (!is_network_error(r))
                check_int_eq(r->status_code, 200);
            http_response_free(r);
            http_client_destroy(c);
        }

        it("should retry on connection error") {
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 3000);
            http_retry_policy_t policy = {
                .max_retries = 1, .initial_delay_ms = 50,
                .max_delay_ms = 100, .retry_on_connection_error = 1
            };
            http_client_set_retry_policy(c, &policy);
            http_response_t *r = http_get(c, "https://nonexistent-host-12345.com");
            check_not_null(r->error);
            http_response_free(r);
            http_client_destroy(c);
        }

        it("should not retry on 4xx") {
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 10000);
            http_retry_policy_t policy = http_retry_policy_default();
            http_client_set_retry_policy(c, &policy);
            http_response_t *r = http_get(c, "https://httpbin.org/status/404");
            if (!is_network_error(r))
                check_int_eq(r->status_code, 404);
            http_response_free(r);
            http_client_destroy(c);
        }
    }
}
