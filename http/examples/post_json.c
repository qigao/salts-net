#include "bdd-for-c.h"
#include "http_client.h"
#include <string.h>

spec("POST JSON Test") {
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

  it("should successfully post JSON data") {
    const char *url = "https://httpbin.org/post";
    const char *json = "{\"name\":\"John\",\"age\":30}";
    
    const char *headers[] = {"Content-Type: application/json",
                             "Accept: application/json"};

    http_response_t *response =
        http_request(client, HTTP_POST, url, headers, 2, json, strlen(json));
    
    check(response != NULL);
    check(response->error == NULL, "Request failed: %s", response->error ? response->error : "unknown error");
    check(response->status_code == 200);
    check(response->body != NULL);
    
    // Verify that the sent JSON is in the response (httpbin echoes it back)
    check(strstr(response->body, "\"name\": \"John\"") != NULL);
    
    http_response_free(response);
  }
}

