#include "bdd-for-c.h"
#include "http_client.h"
#include <string.h>

spec("Login Request Test") {
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

  it("should successfully perform a login request") {
    const char *url = "https://httpbin.org/post";
    const char *login_json = "{\"user\":\"admin\",\"pass\":\"secret\"}";
    
    const char *headers[] = {
        "Content-Type: application/json",
        "Accept: application/json"
    };

    http_response_t *response = http_request(client, HTTP_POST, url, headers, 2, login_json, strlen(login_json));
    
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);
    check(strstr(response->body, "admin") != NULL);
    
    http_response_free(response);
  }
}

