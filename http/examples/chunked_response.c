#include "tinytest.h"
#include "http_client.h"

static int is_network_error(http_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

spec("Chunked Response Test") {

  it("should successfully handle chunked transfer encoding") {
    http_client_t *c = http_client_create("https://httpbin.org");
    http_client_set_timeout(c, 10000);
    http_response_t *r = http_get(c, "https://httpbin.org/stream/5");
    if (!is_network_error(r)) {
      check_int_eq(r->status_code, 200);
      check(r->body_len > 0);
    }
    http_response_free(r);
    http_client_destroy(c);
  }
}
