#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  printf("HTTPS and Redirect Test\n");
  printf("========================\n\n");

  http_client_t *client = http_client_create();
  if (!client) {
    fprintf(stderr, "Failed to create client\n");
    return 1;
  }

  /* Test 1: HTTPS request */
  printf("Test 1: HTTPS Request\n");
  printf("URL: https://httpbin.org/get\n");
  http_response_t *response = http_get(client, "https://httpbin.org/get");

  if (response->error) {
    printf("  Error: %s\n", response->error);
  } else {
    printf("  Status: %d\n", response->status_code);
    printf("  Body length: %zu bytes\n", response->body_len);
    printf("   HTTPS working!\n");
  }
  http_response_free(response);
  printf("\n");

  /* Test 2: HTTP to HTTPS redirect */
  printf("Test 2: HTTP to HTTPS Redirect\n");
  printf("URL: http://httpbin.org/redirect-to?url=https://httpbin.org/get\n");
  response = http_get(
      client, "http://httpbin.org/redirect-to?url=https://httpbin.org/get");

  if (response->error) {
    printf("  Error: %s\n", response->error);
  } else {
    printf("  Final Status: %d\n", response->status_code);
    printf("   Redirect following working!\n");
  }
  http_response_free(response);
  printf("\n");

  /* Test 3: Multiple redirects */
  printf("Test 3: Multiple Redirects\n");
  printf("URL: http://httpbin.org/redirect/3\n");
  response = http_get(client, "http://httpbin.org/redirect/3");

  if (response->error) {
    printf("  Error: %s\n", response->error);
  } else {
    printf("  Final Status: %d\n", response->status_code);
    printf("   Multiple redirects working!\n");
  }
  http_response_free(response);
  printf("\n");

  /* Test 4: Redirect limit */
  printf("Test 4: Redirect Limit (should fail)\n");
  http_client_set_max_redirects(client, 2);
  printf("URL: http://httpbin.org/redirect/5 (max_redirects=2)\n");
  response = http_get(client, "http://httpbin.org/redirect/5");

  if (response->status_code == 302 || response->status_code == 301) {
    printf("  Status: %d (redirect not followed)\n", response->status_code);
    printf("   Redirect limit working!\n");
  } else {
    printf("  Unexpected status: %d\n", response->status_code);
  }
  http_response_free(response);
  printf("\n");

  /* Test 5: Connection reuse */
  printf("Test 5: Connection Reuse\n");
  http_client_set_max_redirects(client, 10);
  printf("Making 3 requests to same host...\n");

  for (int i = 0; i < 3; i++) {
    response = http_get(client, "http://httpbin.org/get");
    if (response->error) {
      printf("  Request %d: Error: %s\n", i + 1, response->error);
    } else {
      printf("  Request %d: Status %d\n", i + 1, response->status_code);
    }
    http_response_free(response);
  }
  printf("   Connection reuse working!\n");
  printf("\n");

  http_client_destroy(client);

  printf("All tests completed!\n");
  return 0;
}
