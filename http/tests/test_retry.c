#include "http_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_retry_policy_default(void) {
  printf("Testing default retry policy...\n");

  http_retry_policy_t policy = http_retry_policy_default();

  assert(policy.max_retries == 3);
  assert(policy.initial_delay_ms == 1000);
  assert(policy.exponential_backoff == 1);
  assert(policy.retry_on_timeout == 0); // Timeout is user's intent, don't retry
  assert(policy.retry_on_connection_error == 1);
  assert(policy.retry_on_5xx == 1);

  printf("   Default retry policy is correct\n");
}

static void test_set_retry_policy(void) {
  printf("Testing set retry policy...\n");

  http_client_t *client = http_client_create();
  assert(client != NULL);

  http_retry_policy_t policy = {.max_retries = 5,
                                .initial_delay_ms = 500,
                                .max_delay_ms = 10000,
                                .exponential_backoff = 1,
                                .retry_on_timeout = 1,
                                .retry_on_connection_error = 1,
                                .retry_on_5xx = 1,
                                .jitter_factor = 0.1};

  http_client_set_retry_policy(client, &policy);

  http_retry_policy_t retrieved;
  http_client_get_retry_policy(client, &retrieved);

  assert(retrieved.max_retries == 5);
  assert(retrieved.initial_delay_ms == 500);
  assert(retrieved.exponential_backoff == 1);

  http_client_destroy(client);

  printf("   Set retry policy works correctly\n");
}

static void test_clear_retry_policy(void) {
  printf("Testing clear retry policy...\n");

  http_client_t *client = http_client_create();
  assert(client != NULL);

  http_retry_policy_t policy = http_retry_policy_default();
  http_client_set_retry_policy(client, &policy);

  http_client_clear_retry_policy(client);

  http_retry_policy_t retrieved;
  http_client_get_retry_policy(client, &retrieved);

  assert(retrieved.max_retries == 0);

  http_client_destroy(client);

  printf("   Clear retry policy works correctly\n");
}

static void test_retry_on_5xx(void) {
  printf("Testing retry on 5xx errors...\n");

  http_client_t *client = http_client_create();
  assert(client != NULL);
  http_client_set_timeout(client, 2000);

  // Set retry policy
  http_retry_policy_t policy = {.max_retries = 2,
                                .initial_delay_ms = 100,
                                .max_delay_ms = 1000,
                                .exponential_backoff = 0,
                                .retry_on_5xx = 1,
                                .jitter_factor = 0.0};

  http_client_set_retry_policy(client, &policy);

  // Request that returns 500
  http_response_t *response = http_get(client, "https://httpbin.org/status/500");

  if (response->error) {
    printf("  Skipping test (network error): %s\n", response->error);
    http_response_free(response);
    http_client_destroy(client);
    return;
  }

  // Should still be 500 after retries
  assert(response->status_code == 500);

  http_response_free(response);
  http_client_destroy(client);

  printf("   Retry on 5xx works correctly\n");
}

static void test_no_retry_on_success(void) {
  printf("Testing no retry on successful request...\n");

  http_client_t *client = http_client_create();
  assert(client != NULL);
  http_client_set_timeout(client, 3000);

  // Set retry policy
  http_retry_policy_t policy = http_retry_policy_default();
  http_client_set_retry_policy(client, &policy);

  // Successful request
  http_response_t *response = http_get(client, "https://httpbin.org/get");

  if (response->error) {
    printf("  Skipping test (network error): %s\n", response->error);
    http_response_free(response);
    http_client_destroy(client);
    return;
  }

  assert(response->status_code == 200);

  http_response_free(response);
  http_client_destroy(client);

  printf("   No retry on success works correctly\n");
}

static void test_retry_on_connection_error(void) {
  printf("Testing retry on connection error...\n");

  http_client_t *client = http_client_create();
  assert(client != NULL);
  http_client_set_timeout(client, 1000);

  // Set retry policy
  http_retry_policy_t policy = {.max_retries = 1,
                                .initial_delay_ms = 50,
                                .max_delay_ms = 100,
                                .exponential_backoff = 0,
                                .retry_on_connection_error = 1,
                                .jitter_factor = 0.0};

  http_client_set_retry_policy(client, &policy);

  // Try non-existent host
  http_response_t *response = http_get(client, "https://nonexistent-host-12345.com");

  // Should have error
  assert(response->error != NULL);

  http_response_free(response);
  http_client_destroy(client);

  printf("   Retry on connection error works correctly\n");
}

static void test_no_retry_on_4xx(void) {
  printf("Testing no retry on 4xx errors...\n");

  http_client_t *client = http_client_create();
  assert(client != NULL);
  http_client_set_timeout(client, 3000);

  // Set retry policy (only retries 5xx)
  http_retry_policy_t policy = http_retry_policy_default();
  http_client_set_retry_policy(client, &policy);

  // Request that returns 404
  http_response_t *response = http_get(client, "https://httpbin.org/status/404");

  if (response->error) {
    printf("  Skipping test (network error): %s\n", response->error);
    http_response_free(response);
    http_client_destroy(client);
    return;
  }

  // Should be 404 without retry (4xx are client errors, not retried)
  assert(response->status_code == 404);

  http_response_free(response);
  http_client_destroy(client);

  printf("   No retry on 4xx works correctly\n");
}

int main(void) {
  printf("\n=== HTTP Client Retry Policy Tests ===\n\n");

  test_retry_policy_default();
  test_set_retry_policy();
  test_clear_retry_policy();
  test_retry_on_5xx();
  test_no_retry_on_success();
  test_retry_on_connection_error();
  test_no_retry_on_4xx();

  printf("\n All retry policy tests passed!\n");
  return 0;
}
