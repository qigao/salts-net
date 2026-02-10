#include "tinytest.h"
#include "http_client.h"
#include <string.h>

spec("JWT Authentication Test") {
  static http_client_t *client = NULL;

  before() {
    client = http_client_create();
    check(client != NULL);
  }

  after() {
    if (client) {
      http_client_destroy(client);
    }
  }

  it("should successfully encode and send JWT token") {
    const char *secret = "v3ry-s3cr3t-sh4r3d-k3y-123456789"; 
    const char *claims = "{\"iss\":\"turbo-client\",\"sub\":\"user_12345\"}";
    
    http_client_set_jwt_auth(client, secret, claims);
    
    http_response_t *response = http_get(client, "https://httpbin.org/bearer");
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);
    
    http_response_free(response);
  }
}
