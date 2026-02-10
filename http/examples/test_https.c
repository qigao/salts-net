#include "http_client.h"
#include "tinytest.h"


spec("HTTPS and Redirect Tests") {
  static http_client_t *client = NULL;

  before() {
    client = http_client_create();
    check(client != NULL, "Failed to create HTTP client");
  }

  after() {
    if (client) {
      http_client_destroy(client);
      client = NULL;
    }
  }

  it("should successfully perform a GET request over HTTPS") {
    http_response_t *response = http_get(client, "https://httpbin.org/get");
    check(response != NULL);
    check(response->error == NULL, "Request failed: %s", response->error ? response->error : "unknown error");
    check(response->status_code == 200);
    check(response->body_len > 0);
    http_response_free(response);
  }

  describe("Redirect Handling") {
    it("should follow redirects") {
      http_response_t *response = http_get(
          client, "https://httpbin.org/redirect-to?url=https://httpbin.org/get");
      check(response != NULL);
      check(response->error == NULL);
      check(response->status_code == 200);
      http_response_free(response);
    }

    it("should follow multiple redirects correctly") {
      http_response_t *response = http_get(client, "https://httpbin.org/redirect/3");
      check(response != NULL);
      check(response->error == NULL);
      check(response->status_code == 200);
      http_response_free(response);
    }

    it("should respect the maximum redirect limit") {
      http_client_set_max_redirects(client, 2);
      http_response_t *response = http_get(client, "https://httpbin.org/redirect/5");
      check(response != NULL);
      // Status code should be 301 or 302 as it stops following
      check(response->status_code == 301 || response->status_code == 302);
      http_response_free(response);
    }
  }

  describe("Connection Lifecycle") {
    it("should reuse connections for multiple requests to the same host") {
      // Ensure we have enough redirects allowed for subsequent tests if any
      http_client_set_max_redirects(client, 10);
      
      for (int i = 0; i < 3; i++) {
        http_response_t *response = http_get(client, "https://httpbin.org/get");
        check(response != NULL);
        check(response->error == NULL, "Request %d failed", i + 1);
        check(response->status_code == 200);
        http_response_free(response);
      }
    }
  }

}

