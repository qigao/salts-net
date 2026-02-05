#include "bdd-for-c.h"
#include "http_client.h"
#include <time.h>

// Request logging interceptor
static int log_request(http_request_context_t* ctx) {
    // Interceptor logic
    return 0;  // Continue with request
}

// Response logging interceptor
static void log_response(http_response_context_t* ctx) {
    // Interceptor logic
}

spec("Interceptors Test") {
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

  it("should successfully use request and response interceptors") {
    http_client_add_request_interceptor(client, log_request, NULL);
    http_client_add_response_interceptor(client, log_response, NULL);
    
    http_response_t *response = http_get(client, "https://httpbin.org/get");
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);
    
    http_response_free(response);
  }

  it("should successfully clear interceptors") {
    http_client_clear_interceptors(client);
    
    http_response_t *response = http_get(client, "https://httpbin.org/get");
    check(response != NULL);
    check(response->error == NULL);
    
    http_response_free(response);
  }
}

