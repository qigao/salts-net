#include "tinytest.h"
#include "http_client.h"
#include <string.h>

static int is_network_error(http_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

spec("HTTPS and Redirect Tests") {

  it("should successfully perform a GET request over HTTPS") {
    http_client_t *c = http_client_create();
    http_client_set_timeout(c, 10000);
    http_response_t *r = http_get(c, "https://httpbin.org/get");
    if (!is_network_error(r)) {
      check_int_eq(r->status_code, 200);
      check(r->body_len > 0);
    }
    http_response_free(r);
    http_client_destroy(c);
  }

  describe("Redirect Handling") {
    it("should follow redirects") {
      http_client_t *c = http_client_create();
      http_client_set_timeout(c, 10000);
      http_response_t *r = http_get(c,
          "https://httpbin.org/redirect-to?url=https://httpbin.org/get");
      if (!is_network_error(r))
        check_int_eq(r->status_code, 200);
      http_response_free(r);
      http_client_destroy(c);
    }

    it("should follow multiple redirects correctly") {
      http_client_t *c = http_client_create();
      http_client_set_timeout(c, 15000);
      http_response_t *r = http_get(c, "https://httpbin.org/redirect/3");
      if (!is_network_error(r))
        check_int_eq(r->status_code, 200);
      http_response_free(r);
      http_client_destroy(c);
    }

    it("should respect the maximum redirect limit") {
      http_client_t *c = http_client_create();
      http_client_set_timeout(c, 15000);
      http_client_set_max_redirects(c, 2);
      http_response_t *r = http_get(c, "https://httpbin.org/redirect/5");
      if (!is_network_error(r))
        check(r->status_code == 301 || r->status_code == 302);
      http_response_free(r);
      http_client_destroy(c);
    }
  }

  describe("Connection Lifecycle") {
    it("should reuse connections for multiple requests to the same host") {
      http_client_t *c = http_client_create();
      http_client_set_timeout(c, 10000);
      for (int i = 0; i < 3; i++) {
        http_response_t *r = http_get(c, "https://httpbin.org/get");
        if (is_network_error(r)) {
          http_response_free(r);
          http_client_destroy(c);
          return;
        }
        check_int_eq(r->status_code, 200);
        http_response_free(r);
      }
      http_client_destroy(c);
    }
  }
}
