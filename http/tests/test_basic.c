/**
 * test_basic.c - Basic HTTP client tests
 */

#include "http_client.h"
#include <unity.h>
#include <string.h>

void setUp(void) { /* Called before each test */ }

void tearDown(void) { /* Called after each test */ }

void test_client_creation(void) {
  http_client_t *client = http_client_create();
  TEST_ASSERT_NOT_NULL(client);
  http_client_destroy(client);
}

void test_client_destroy_null(void) {
  /* Should not crash */
  http_client_destroy(NULL);
  TEST_PASS();
}

void test_set_timeout(void) {
  http_client_t *client = http_client_create();
  TEST_ASSERT_NOT_NULL(client);

  http_client_set_timeout(client, 5000);
  http_client_set_timeout(client, 0);
  http_client_set_timeout(client, -1); /* Should handle gracefully */

  http_client_destroy(client);
}

void test_set_user_agent(void) {
  http_client_t *client = http_client_create();
  TEST_ASSERT_NOT_NULL(client);

  http_client_set_user_agent(client, "TestAgent/1.0");
  http_client_set_user_agent(client, "");
  http_client_set_user_agent(client, NULL); /* Should handle gracefully */

  http_client_destroy(client);
}

void test_follow_redirects(void) {
  http_client_t *client = http_client_create();
  TEST_ASSERT_NOT_NULL(client);

  http_client_follow_redirects(client, 1);
  http_client_follow_redirects(client, 0);

  http_client_destroy(client);
}

void test_set_max_redirects(void) {
  http_client_t *client = http_client_create();
  TEST_ASSERT_NOT_NULL(client);

  http_client_set_max_redirects(client, 5);
  http_client_set_max_redirects(client, 0);
  http_client_set_max_redirects(client, 100);
  http_client_set_max_redirects(client, -1); /* Should be rejected */

  http_client_destroy(client);
}

void test_response_free_null(void) {
  /* Should not crash */
  http_response_free(NULL);
  TEST_PASS();
}

void test_invalid_url(void) {
  http_client_t *client = http_client_create();
  TEST_ASSERT_NOT_NULL(client);

  /* NULL URL should return error response */
  http_response_t *response = http_get(client, NULL);
  TEST_ASSERT_NOT_NULL(response);
  TEST_ASSERT_NOT_NULL(response->error);

  http_response_free(response);
  http_client_destroy(client);
}

void test_malformed_url(void) {
  http_client_t *client = http_client_create();
  TEST_ASSERT_NOT_NULL(client);

  /* Malformed URLs should be handled gracefully */
  http_response_t *response = http_get(client, "not-a-url");
  TEST_ASSERT_NOT_NULL(response);
  /* Should either parse or return error */

  http_response_free(response);
  http_client_destroy(client);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_client_creation);
  RUN_TEST(test_client_destroy_null);
  RUN_TEST(test_set_timeout);
  RUN_TEST(test_set_user_agent);
  RUN_TEST(test_follow_redirects);
  RUN_TEST(test_set_max_redirects);
  RUN_TEST(test_response_free_null);
  RUN_TEST(test_invalid_url);
  RUN_TEST(test_malformed_url);

  return UNITY_END();
}
