#include "bdd-for-c.h"
#include "http_client.h"

spec("Chunked Response Test") {
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

  it("should successfully handle chunked transfer encoding") {
    http_response_t *response = http_get(client, "https://httpbin.org/stream/5");
    
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);
    check(response->body_len > 0);
    
    http_response_free(response);
  }
}

