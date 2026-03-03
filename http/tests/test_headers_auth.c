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

spec("http headers and auth") {

    describe("response headers") {

        it("should parse content type") {
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 10000);
            http_response_t *r = http_get(c, "https://httpbin.org/json");
            if (!is_network_error(r)) {
                check_int_eq(r->status_code, 200);
                char *ct = http_response_content_type(r);
                check(ct != NULL && strstr(ct, "application/json") != NULL);
                free(ct);
                check_int_eq(http_response_is_json(r), 1);
                check_int_eq(http_response_is_html(r), 0);
                check_int_eq(http_response_has_header(r, "Content-Type"), 1);
                check_int_eq(http_response_has_header(r, "NonExistent-Header"), 0);
            }
            http_response_free(r);
            http_client_destroy(c);
        }
    }

    describe("basic auth") {

        it("should authenticate with correct credentials") {
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 10000);
            http_client_set_basic_auth(c, "user", "passwd");
            http_response_t *r = http_get(c, "https://httpbin.org/basic-auth/user/passwd");
            if (!is_network_error(r)) {
                check_int_eq(r->status_code, 200);
                check_not_null(r->body);
                check(strstr(r->body, "authenticated") != NULL);
            }
            http_response_free(r);
            http_client_destroy(c);
        }
    }

    describe("bearer token") {

        it("should authenticate with token") {
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 10000);
            http_client_set_bearer_token(c, "my-secret-token");
            http_response_t *r = http_get(c, "https://httpbin.org/bearer");
            if (!is_network_error(r)) {
                check_int_eq(r->status_code, 200);
                check_not_null(r->body);
                check(strstr(r->body, "authenticated") != NULL);
            }
            http_response_free(r);
            http_client_destroy(c);
        }
    }

    describe("clear auth") {

        it("should clear credentials") {
            http_client_t *c = http_client_create();
            http_client_set_timeout(c, 10000);
            http_client_set_basic_auth(c, "user", "passwd");
            http_client_clear_auth(c);
            http_response_t *r = http_get(c, "https://httpbin.org/get");
            if (!is_network_error(r)) {
                check_int_eq(r->status_code, 200);
            }
            http_response_free(r);
            http_client_destroy(c);
        }
    }

    describe("error codes") {

        it("should set error code for invalid url") {
            http_client_t *c = http_client_create();
            http_response_t *r = http_get(c, "not-a-valid-url");
            check_not_null(r->error);
            check_int_ne(r->error_code, HTTP_ERROR_NONE);
            http_response_free(r);
            http_client_destroy(c);
        }

        it("should set error code for non-existent host") {
            http_client_t *c = http_client_create();
            http_client_set_connect_timeout(c, 1000);
            http_response_t *r = http_get(c, "https://this-host-definitely-does-not-exist-12345.com");
            check_not_null(r->error);
            check_int_ne(r->error_code, HTTP_ERROR_NONE);
            http_response_free(r);
            http_client_destroy(c);
        }
    }
}
