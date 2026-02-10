#include "tinytest.h"
#include "http_client.h"

spec("Response Headers Test") {
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

  it("should successfully retrieve and check response headers for JSON") {
    http_response_t *response = http_get(client, "https://httpbin.org/json");
    
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);

    char *content_type = http_response_content_type(response);
    check(content_type != NULL);
    check(strstr(content_type, "application/json") != NULL);
    free(content_type);

    check(http_response_content_length(response) > 0);
    
    char *server = http_response_get_header(response, "Server");
    check(server != NULL);
    free(server);

    check(http_response_is_json(response) == 1);
    check(http_response_is_html(response) == 0);
    
    http_response_free(response);
  }

  it("should successfully retrieve and check response headers for HTML") {
    http_response_t *response = http_get(client, "https://httpbin.org/html");
    
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);

    check(http_response_is_json(response) == 0);
    check(http_response_is_html(response) == 1);
    
    http_response_free(response);
  }
}

