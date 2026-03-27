#include <platform.h>
#include "tinytest.h"
#include "http_client.h"
#include "../src/http_client_internal_h.h"
#include <string.h>
#include <stdlib.h>

spec("http client basic") {

    describe("client lifecycle") {

        it("should create and destroy") {
            http_client_t *client = http_client_create("http://localhost:8080");
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

        before_each() {
            client = http_client_create("http://localhost:8080");
        }
        after_each() {
            http_client_destroy(client);
        }

        it("should set timeout") {
            http_client_set_timeout(client, 5000);
            http_client_set_connect_timeout(client, 3000);
            http_client_set_read_timeout(client, 4000);
            check(1);
        }

        it("should set user agent") {
            http_client_set_user_agent(client, "TestAgent/1.0");
            http_client_set_user_agent(client, NULL);
            check(1);
        }

        it("should get base url") {
            check_str_eq(http_client_get_base_url(client), "http://localhost:8080");
        }

        it("should set follow redirects") {
            http_client_follow_redirects(client, 1);
            http_client_follow_redirects(client, 0);
            check(1);
        }

        it("should set max redirects") {
            http_client_set_max_redirects(client, 5);
            http_client_set_max_redirects(client, 0);
            check(1);
        }
    }

    describe("headers & auth") {
        static http_client_t *client;

        before_each() {
            client = http_client_create("http://localhost:8080");
        }
        after_each() {
            http_client_destroy(client);
        }

        it("should manage default headers") {
            http_client_set_default_header(client, "X-Test", "Value");
            check(http_client_has_default_header(client, "X-Test"));
            http_client_remove_default_header(client, "X-Test");
            check(!http_client_has_default_header(client, "X-Test"));

            http_client_set_default_header(client, "X-Multiple", "1");
            http_client_set_default_header(client, "X-Multiple", "2");
            http_client_clear_default_headers(client);
            check(!http_client_has_default_header(client, "X-Multiple"));
        }

        it("should set auth") {
            http_client_set_basic_auth(client, "user", "pass");
            http_client_set_bearer_token(client, "token123");
            http_client_set_jwt_auth(client, "secret", "{\"sub\":\"123\"}");
            http_client_clear_auth(client);
            check(1);
        }

        it("should clear stale auth on invalid jwt config") {
            http_client_set_bearer_token(client, "token123");
            check_not_null(client->auth_header);

            http_client_set_jwt_auth(client, "secret", "{\"sub\":");

            check_null(client->auth_header);
        }

        it("should set cookie jar") {
            http_client_set_cookie_jar(client, NULL);
            check(http_client_get_cookie_jar(client) == NULL);
        }

        it("should reject oversized proxy config without truncation") {
            char host[300];

            memset(host, 'a', sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';

            http_client_set_proxy(client, host, 1080, NULL, NULL);

            check_not_null(client->proxy_config);
            check_str_eq(client->proxy_config->host, "");
            check_int_eq(client->proxy_config->port, 0);
            check_int_eq(client->proxy_config->auth_required, 0);
        }
    }

    describe("advanced configuration") {
        static http_client_t *client;

        before_each() {
            client = http_client_create("http://localhost:8080");
        }
        after_each() {
            http_client_destroy(client);
        }

        it("should set retry policy") {
            http_retry_policy_t policy = { .max_retries = 3 };
            http_client_set_retry_policy(client, &policy);
            http_retry_policy_t check_policy;
            http_client_get_retry_policy(client, &check_policy);
            check_int_eq(check_policy.max_retries, 3);
            http_client_clear_retry_policy(client);
        }

        it("should set rate limit") {
            http_rate_limit_t limit = { .requests_per_second = 10, .burst_size = 5 };
            http_client_set_rate_limit(client, &limit);
            check(http_client_has_rate_limit(client));
            http_client_clear_rate_limit(client);
            check(!http_client_has_rate_limit(client));
        }

        it("should reset stats") {
            http_client_reset_stats(client);
            http_client_stats_t stats;
            http_client_get_stats(client, &stats);
            check_int_eq((int)stats.total_requests, 0);
        }
    }

    describe("error handling") {

        it("should handle response free NULL") {
            http_response_free(NULL);
            check(1);
        }

        it("should return error for NULL url") {
            http_client_t *client = http_client_create("http://localhost:8080");
            http_response_t *resp = http_get(client, NULL);
            check_not_null(resp);
            if (resp) {
                check_int_eq(resp->error_code, HTTP_ERROR_INVALID_URL);
                http_response_free(resp);
            }
            http_client_destroy(client);
        }

        it("should handle malformed url") {
            http_client_t *c = http_client_create("http://localhost:8080");
            http_response_t *r = http_get(c, "not-a-url");
            check_not_null(r);
            http_response_free(r);
            http_client_destroy(c);
        }
    }
}
