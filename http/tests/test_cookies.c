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

spec("http cookies") {

    describe("cookie jar") {

        it("should create and destroy") {
            http_cookie_jar_t *jar = http_cookie_jar_create();
            check_not_null(jar);
            check_size_eq(http_cookie_jar_count(jar), 0);
            http_cookie_jar_destroy(jar);
        }

        it("should set and get cookies") {
            http_cookie_jar_t *jar = http_cookie_jar_create();
            http_cookie_jar_set(jar, "session", "abc123");
            check_size_eq(http_cookie_jar_count(jar), 1);
            http_cookie_jar_set(jar, "user_id", "42");
            check_size_eq(http_cookie_jar_count(jar), 2);
            check_str_eq(http_cookie_jar_get(jar, "session"), "abc123");
            check_str_eq(http_cookie_jar_get(jar, "user_id"), "42");
            check_null(http_cookie_jar_get(jar, "nonexistent"));
            http_cookie_jar_destroy(jar);
        }

        it("should update existing cookie") {
            http_cookie_jar_t *jar = http_cookie_jar_create();
            http_cookie_jar_set(jar, "session", "abc123");
            http_cookie_jar_set(jar, "session", "xyz789");
            check_str_eq(http_cookie_jar_get(jar, "session"), "xyz789");
            check_size_eq(http_cookie_jar_count(jar), 1);
            http_cookie_jar_destroy(jar);
        }

        it("should remove cookies") {
            http_cookie_jar_t *jar = http_cookie_jar_create();
            http_cookie_jar_set(jar, "a", "1");
            http_cookie_jar_set(jar, "b", "2");
            http_cookie_jar_remove(jar, "a");
            check_size_eq(http_cookie_jar_count(jar), 1);
            check_null(http_cookie_jar_get(jar, "a"));
            check_not_null(http_cookie_jar_get(jar, "b"));
            http_cookie_jar_destroy(jar);
        }

        it("should clear all cookies") {
            http_cookie_jar_t *jar = http_cookie_jar_create();
            http_cookie_jar_set(jar, "a", "1");
            http_cookie_jar_set(jar, "b", "2");
            http_cookie_jar_set(jar, "c", "3");
            http_cookie_jar_clear(jar);
            check_size_eq(http_cookie_jar_count(jar), 0);
            http_cookie_jar_destroy(jar);
        }
    }

    describe("automatic handling") {

        it("should store cookies from Set-Cookie header") {
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 10000);
            http_cookie_jar_t *jar = http_cookie_jar_create();
            http_client_set_cookie_jar(c, jar);
            http_response_t *r = http_get(c, "https://httpbin.org/cookies/set?test=value123");
            if (!is_network_error(r)) {
                const char *val = http_cookie_jar_get(jar, "test");
                check(val != NULL && strcmp(val, "value123") == 0);
            }
            http_response_free(r);
            http_client_destroy(c);
            http_cookie_jar_destroy(jar);
        }

        it("should work without cookie jar") {
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 10000);
            http_response_t *r = http_get(c, "https://httpbin.org/get");
            if (!is_network_error(r)) {
                check_int_eq(r->status_code, 200);
            }
            http_response_free(r);
            http_client_destroy(c);
        }
    }
}
