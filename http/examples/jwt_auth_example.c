#include "tinytest.h"
#include "http_client.h"

static int is_network_error(http_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

spec("JWT Authentication Test") {

  it("should successfully encode and send JWT token") {
    http_client_t *c = http_client_create("https://httpbin.org");
    http_client_set_timeout(c, 10000);
    const char *secret = "v3ry-s3cr3t-sh4r3d-k3y-123456789";
    const char *claims = "{\"iss\":\"turbo-client\",\"sub\":\"user_12345\"}";
    http_client_set_jwt_auth(c, secret, claims);
    http_response_t *r = http_get(c, "https://httpbin.org/bearer");
    if (!is_network_error(r))
      check_int_eq(r->status_code, 200);
    http_response_free(r);
    http_client_destroy(c);
  }
}
