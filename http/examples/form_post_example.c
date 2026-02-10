#include "tinytest.h"
#include "http_client.h"

spec("Form Post Test") {
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

  it("should successfully perform a URL-encoded form POST") {
    http_params_t *params = http_params_create();
    http_params_add(params, "name", "John Doe");
    http_params_add(params, "message", "Hello from HTTP client!");
    
    http_response_t *response = http_post_form(client, 
                                               "https://httpbin.org/post",
                                               params);
    
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);
    check(strstr(response->body, "John Doe") != NULL);
    
    http_response_free(response);
    http_params_free(params);
  }

  it("should successfully build URLs with query parameters") {
    http_params_t *query = http_params_create();
    http_params_add(query, "search", "http client");
    http_params_add(query, "page", "1");
    
    char *url = http_build_url("https://httpbin.org/get", query);
    check(url != NULL);
    check(strstr(url, "search=http+client") != NULL);
    
    http_response_t *response = http_get(client, url);
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);
    
    http_response_free(response);
    http_params_free(query);
    free(url);
  }
}

