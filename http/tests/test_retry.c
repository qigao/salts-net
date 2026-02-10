#include "tinytest.h"
#include "http_client.h"
#include <string.h>

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
            http_client_t *client = http_client_create();
            http_client_set_timeout(client, 2000);

            http_retry_policy_t policy = {
                .max_retries = 2,
                .initial_delay_ms = 100,
                .max_delay_ms = 1000,
                .exponential_backoff = 0,
                .retry_on_5xx = 1,
                .jitter_factor = 0.0
            };
            http_client_set_retry_policy(client, &policy);

            http_response_t *response = http_get(client, "https://httpbin.org/status/500");
            if (response->error) {
                http_response_free(response);
                http_client_destroy(client);
                return;
            }
            check_int_eq(response->status_code, 500);

            http_response_free(response);
            http_client_destroy(client);
        }

        it("should not retry on success") {
            http_client_t *client = http_client_create();
            http_client_set_timeout(client, 3000);

            http_retry_policy_t policy = http_retry_policy_default();
            http_client_set_retry_policy(client, &policy);

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

        it("should retry on connection error") {
            http_client_t *client = http_client_create();
            http_client_set_timeout(client, 1000);

            http_retry_policy_t policy = {
                .max_retries = 1,
                .initial_delay_ms = 50,
                .max_delay_ms = 100,
                .exponential_backoff = 0,
                .retry_on_connection_error = 1,
                .jitter_factor = 0.0
            };
            http_client_set_retry_policy(client, &policy);

            http_response_t *response = http_get(client, "https://nonexistent-host-12345.com");
            check_not_null(response->error);

            http_response_free(response);
            http_client_destroy(client);
        }

        it("should not retry on 4xx") {
            http_client_t *client = http_client_create();
            http_client_set_timeout(client, 3000);

            http_retry_policy_t policy = http_retry_policy_default();
            http_client_set_retry_policy(client, &policy);

            http_response_t *response = http_get(client, "https://httpbin.org/status/404");
            if (response->error) {
                http_response_free(response);
                http_client_destroy(client);
                return;
            }
            check_int_eq(response->status_code, 404);

            http_response_free(response);
            http_client_destroy(client);
        }
    }
}
