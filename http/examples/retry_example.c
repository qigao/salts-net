#include "bdd-for-c.h"
#include "http_client.h"
#include <time.h>

spec("Retry Policy Test") {
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

  it("should fail immediately on 500 error when no retry policy is set") {
    http_client_clear_retry_policy(client);
    http_response_t *response = http_get(client, "https://httpbin.org/status/500");
    
    check(response != NULL);
    check(response->status_code == 500);
    
    http_response_free(response);
  }

  it("should successfully apply a retry policy on 500 errors") {
    http_retry_policy_t policy = http_retry_policy_default();
    policy.max_retries = 2;
    policy.initial_delay_ms = 100;
    policy.retry_on_5xx = 1;
    
    http_client_set_retry_policy(client, &policy);
    
    clock_t start = clock();
    http_response_t *response = http_get(client, "https://httpbin.org/status/500");
    clock_t end = clock();
    
    double elapsed = ((double)(end - start)) / CLOCKS_PER_SEC;
    
    check(response != NULL);
    check(response->status_code == 500);
    check(elapsed >= 0.2); // At least 2 retries with 100ms delay each
    
    http_response_free(response);
  }

  it("should successfully retry on connection errors") {
    http_retry_policy_t policy = http_retry_policy_default();
    policy.retry_on_connection_error = 1;
    policy.max_retries = 1;
    policy.initial_delay_ms = 100;
    http_client_set_retry_policy(client, &policy);
    
    http_response_t *response = http_get(client, "https://this-host-does-not-exist-12345.com");
    check(response != NULL);
    check(response->error != NULL);
    
    http_response_free(response);
  }
}

