
#include "tinytest.h"
#include "http_client.h"

spec("Authentication Test") {
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

  it("should successfully perform Basic Authentication") {
    http_client_set_basic_auth(client, "user", "passwd");
    http_response_t *response = http_get(client, "https://httpbin.org/basic-auth/user/passwd");
    
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);
    check(strstr(response->body, "\"authenticated\": true") != NULL);
    
    http_response_free(response);
  }

  it("should successfully perform Bearer Token Authentication") {
    http_client_clear_auth(client);
    http_client_set_bearer_token(client, "my-secret-token-12345");
    
    http_response_t *response = http_get(client, "https://httpbin.org/bearer");
    
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);
    check(strstr(response->body, "my-secret-token-12345") != NULL);
    
    http_response_free(response);
  }

  it("should successfully perform a request without authentication") {
    http_client_clear_auth(client);
    http_response_t *response = http_get(client, "https://httpbin.org/get");
    
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);
    
    http_response_free(response);
  }
}

