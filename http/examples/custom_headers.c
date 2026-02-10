#include "tinytest.h"
#include "http_client.h"

spec("Custom Headers Test") {
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

  it("should successfully send custom headers") {
    const char *url = "https://httpbin.org/headers";
    
    http_client_set_user_agent(client, "MyApp/2.0");
    
    const char *headers[] = {
        "X-Custom-Header: MyValue",
        "X-Test-Request-ID: 12345",
        "Accept: application/json"
    };

    http_response_t *response = http_request(client, HTTP_GET, url,
                                             headers, 3,
                                             NULL, 0);
    
    check(response != NULL);
    check(response->error == NULL, "Request failed: %s", response->error ? response->error : "unknown error");
    check(response->status_code == 200);
    check(response->body != NULL);
    
    // Verify that our headers were received by server
    if (strstr(response->body, "12345") == NULL) {
        printf("Response Body:\n%s\n", response->body);
    }
    check(strstr(response->body, "MyValue") != NULL);
    check(strstr(response->body, "12345") != NULL);
    check(strstr(response->body, "MyApp/2.0") != NULL);
    
    http_response_free(response);
  }
}

