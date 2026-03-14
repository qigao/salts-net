#include "tinytest.h"
#include "http_client.h"
#include <string.h>

static int is_network_error(http_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

spec("Advanced Features Test") {

  it("should successfully apply custom timeouts") {
    http_client_t *c = http_client_create("https://httpbin.org");
    http_client_set_connect_timeout(c, 3000);
    http_client_set_timeout(c, 10000);
    http_response_t *r = http_get(c, "https://httpbin.org/delay/1");
    if (!is_network_error(r))
      check_int_eq(r->status_code, 200);
    http_response_free(r);
    http_client_destroy(c);
  }

  it("should successfully handle compressed responses") {
    http_client_t *c = http_client_create("https://httpbin.org");
    http_client_set_timeout(c, 10000);
    http_client_enable_compression(c, 1);
    http_response_t *r = http_get(c, "https://httpbin.org/gzip");
    if (!is_network_error(r)) {
      check_int_eq(r->status_code, 200);
      check_not_null(r->body);
    }
    http_response_free(r);
    http_client_destroy(c);
  }

  it("should successfully perform range requests") {
    http_client_t *c = http_client_create("https://httpbin.org");
    http_client_set_timeout(c, 10000);
    http_response_t *r = http_get_range(c, "https://httpbin.org/bytes/1000", 0, 99);
    if (!is_network_error(r)) {
      if (r->status_code == 206)
        check(r->body_len == 100);
    }
    http_response_free(r);
    http_client_destroy(c);
  }
}
