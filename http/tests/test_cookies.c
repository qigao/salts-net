#include "tinytest.h"
#include "http_client.h"
#include <string.h>

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
            http_client_t *client = http_client_create();
            http_cookie_jar_t *jar = http_cookie_jar_create();
            http_client_set_cookie_jar(client, jar);
            check(http_client_get_cookie_jar(client) == jar, "jar should be attached");

            http_response_t *response = http_get(client, "https://httpbin.org/cookies/set?test=value123");

            if (response->error) {
                http_response_free(response);
                http_cookie_jar_destroy(jar);
                http_client_destroy(client);
                return;
            }

            http_response_free(response);

            const char *test_cookie = http_cookie_jar_get(jar, "test");
            if (test_cookie)
                check_str_eq(test_cookie, "value123");

            http_cookie_jar_destroy(jar);
            http_client_destroy(client);
        }

        it("should work without cookie jar") {
            http_client_t *client = http_client_create();
            check_null(http_client_get_cookie_jar(client));

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
    }
}
