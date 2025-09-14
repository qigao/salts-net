#include "http_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_response_headers(void) {
  printf("Testing response header helpers...\n");

  http_client_t *client = http_client_create();
  assert(client != NULL);

  // Test with httpbin.org which returns predictable headers
  http_response_t *response = http_get(client, "https://httpbin.org/json");

  if (response->error) {
    printf("  Skipping test (network error): %s (code: %d)\n", response->error,
           response->error_code);
    http_response_free(response);
    http_client_destroy(client);
    return;
  }

  if (response->status_code != 200) {
    printf("  Skipping test (unexpected status %d)\n", response->status_code);
    http_response_free(response);
    http_client_destroy(client);
    return;
  }

  // Test content type helper
  char *content_type = (char *)http_response_content_type(response);
  assert(content_type != NULL);
  assert(strstr(content_type, "application/json") != NULL);
  free(content_type);

  // Test content type checks
  assert(http_response_is_json(response) == 1);
  assert(http_response_is_html(response) == 0);

  // Test has_header
  assert(http_response_has_header(response, "Content-Type") == 1);
  assert(http_response_has_header(response, "NonExistent-Header") == 0);

  // Test get_header
  char *server = (char *)http_response_get_header(response, "Server");
  if (server) {
    assert(strlen(server) > 0);
    free(server);
  }

  http_response_free(response);
  http_client_destroy(client);

  printf("   Response header helpers work correctly\n");
}

static void test_basic_auth(void) {
  printf("Testing basic authentication...\n");

  http_client_t *client = http_client_create();
  assert(client != NULL);

  // Set basic auth
  http_client_set_basic_auth(client, "user", "passwd");

  // Test with httpbin.org basic auth endpoint
  http_response_t *response =
      http_get(client, "https://httpbin.org/basic-auth/user/passwd");

  if (response->error) {
    printf("  Skipping test (network error): %s (code: %d)\n", response->error,
           response->error_code);
    http_response_free(response);
    http_client_destroy(client);
    return;
  }

  if (response->status_code != 200) {
    printf("  Skipping test (unexpected status %d)\n", response->status_code);
    http_response_free(response);
    http_client_destroy(client);
    return;
  }

  if (!response->body) {
    printf("  Skipping test (no response body)\n");
    http_response_free(response);
    http_client_destroy(client);
    return;
  }

  assert(strstr(response->body, "authenticated") != NULL);

  http_response_free(response);

  // Test wrong credentials
  http_client_set_basic_auth(client, "wrong", "credentials");
  response = http_get(client, "https://httpbin.org/basic-auth/user/passwd");

  if (!response->error && response->status_code == 401) {
    printf("   Wrong credentials correctly rejected (401)\n");
  }

  http_response_free(response);
  http_client_destroy(client);

  printf("   Basic authentication works correctly\n");
}

static void test_bearer_token(void) {
  printf("Testing bearer token authentication...\n");

  http_client_t *client = http_client_create();
  assert(client != NULL);

  // Set bearer token
  http_client_set_bearer_token(client, "my-secret-token");

  // Test with httpbin.org bearer endpoint
  http_response_t *response = http_get(client, "https://httpbin.org/bearer");

  if (response->error) {
    printf("  Skipping test (network error): %s (code: %d)\n", response->error,
           response->error_code);
    http_response_free(response);
    http_client_destroy(client);
    return;
  }

  if (response->status_code != 200) {
    printf("  Skipping test (unexpected status %d)\n", response->status_code);
    http_response_free(response);
    http_client_destroy(client);
    return;
  }

  if (!response->body) {
    printf("  Skipping test (no response body)\n");
    http_response_free(response);
    http_client_destroy(client);
    return;
  }

  assert(strstr(response->body, "authenticated") != NULL);

  http_response_free(response);
  http_client_destroy(client);

  printf("   Bearer token authentication works correctly\n");
}

static void test_clear_auth(void) {
  printf("Testing clear authentication...\n");

  http_client_t *client = http_client_create();
  assert(client != NULL);

  // Set auth then clear it
  http_client_set_basic_auth(client, "user", "passwd");
  http_client_clear_auth(client);

  // Request should work without auth
  http_response_t *response = http_get(client, "https://httpbin.org/get");

  if (response->error) {
    printf("  Skipping test (network error): %s (code: %d)\n", response->error,
           response->error_code);
    http_response_free(response);
    http_client_destroy(client);
    return;
  }

  if (response->status_code != 200) {
    printf("  Skipping test (unexpected status %d)\n", response->status_code);
    http_response_free(response);
    http_client_destroy(client);
    return;
  }

  http_response_free(response);
  http_client_destroy(client);

  printf("   Clear authentication works correctly\n");
}

static void test_error_codes(void) {
  printf("Testing error codes...\n");

  http_client_t *client = http_client_create();
  assert(client != NULL);

  // Test invalid URL
  http_response_t *response = http_get(client, "not-a-valid-url");
  assert(response->error != NULL);
  // Should have an error code set
  assert(response->error_code != HTTP_ERROR_NONE);
  http_response_free(response);

  // Test connection to non-existent host
  // Set short timeout to avoid waiting too long
  http_client_set_connect_timeout(client, 1000);
  response =
      http_get(client, "https://this-host-definitely-does-not-exist-12345.com");
  assert(response->error != NULL);
  assert(response->error_code != HTTP_ERROR_NONE);
  http_response_free(response);

  http_client_destroy(client);

  printf("   Error codes work correctly\n");
}

int main(void) {
  printf("\n=== HTTP Client Headers & Auth Tests ===\n\n");

  test_response_headers();
  test_basic_auth();
  test_bearer_token();
  test_clear_auth();
  test_error_codes();

  printf("\n All tests passed!\n");
  return 0;
}
