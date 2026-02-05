#include "bdd-for-c.h"
#include "http_client.h"

spec("Simple GET Test") {
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

  it("should successfully perform a basic GET request") {
    const char *url = "http://httpbin.org/get";
    http_response_t *response = http_get(client, url);
    
    check(response != NULL);
    check(response->error == NULL, "Request failed: %s", response->error ? response->error : "unknown error");
    check(response->status_code == 200);
    check(response->body != NULL);
    check(response->body_len > 0);
    
    http_response_free(response);
  }
}

