/**
 * test_requests.c - HTTP request method tests
 */

#include "http_client.h"
#include <unity.h>
#include <string.h>

static http_client_t *client = NULL;

void setUp(void) {
  client = http_client_create();
  TEST_ASSERT_NOT_NULL(client);
}

void tearDown(void) {
  http_client_destroy(client);
  client = NULL;
}

void test_get_request_structure(void) {
  /* Test that client and request functions don't crash with NULL */
  TEST_ASSERT_NOT_NULL(client);

  /* Test NULL URL handling */
  http_response_t *response = http_get(client, NULL);
  TEST_ASSERT_NOT_NULL(response);
  TEST_ASSERT_NOT_NULL(response->error);
  http_response_free(response);
}

void test_post_request_structure(void) {
  /* Test POST with NULL URL */
  const char *body = "{\"test\":\"data\"}";
  http_response_t *response = http_post(client, NULL, body, strlen(body));
  TEST_ASSERT_NOT_NULL(response);
  TEST_ASSERT_NOT_NULL(response->error);
  http_response_free(response);
}

void test_post_empty_body(void) {
  /* Test POST with empty body and NULL URL */
  http_response_t *response = http_post(client, NULL, NULL, 0);
  TEST_ASSERT_NOT_NULL(response);
  TEST_ASSERT_NOT_NULL(response->error);
  http_response_free(response);
}

void test_custom_headers(void) {
  /* Test that custom headers don't crash */
  const char *headers[] = {"X-Custom-Header: value1",
                           "X-Another-Header: value2"};

  http_response_t *response =
      http_request(client, HTTP_GET, NULL, headers, 2, NULL, 0);
  TEST_ASSERT_NOT_NULL(response);
  TEST_ASSERT_NOT_NULL(response->error);
  http_response_free(response);
}

void test_request_with_body_and_headers(void) {
  /* Test request with both headers and body */
  const char *headers[] = {"Content-Type: application/json"};
  const char *body = "{\"key\":\"value\"}";

  http_response_t *response =
      http_request(client, HTTP_POST, NULL, headers, 1, body, strlen(body));
  TEST_ASSERT_NOT_NULL(response);
  TEST_ASSERT_NOT_NULL(response->error);
  http_response_free(response);
}

void test_different_http_methods(void) {
  /* Test that different HTTP methods are accepted */
  http_method_t methods[] = {HTTP_GET,    HTTP_POST, HTTP_PUT,
                             HTTP_DELETE, HTTP_HEAD, HTTP_PATCH};

  for (int i = 0; i < 6; i++) {
    http_response_t *response =
        http_request(client, methods[i], NULL, NULL, 0, NULL, 0);
    TEST_ASSERT_NOT_NULL(response);
    TEST_ASSERT_NOT_NULL(response->error);
    http_response_free(response);
  }
}

void test_url_with_query_params(void) {
  /* Test URL parsing with query params (will fail but not crash) */
  http_client_set_timeout(client, 100); /* Very short timeout */
  http_response_t *response =
      http_get(client, "http://127.0.0.1:1/test?param1=value1&param2=value2");
  TEST_ASSERT_NOT_NULL(response);
  http_response_free(response);
}

void test_url_with_fragment(void) {
  /* Test URL parsing with fragment (will fail but not crash) */
  http_client_set_timeout(client, 100); /* Very short timeout */
  http_response_t *response =
      http_get(client, "http://127.0.0.1:1/test#fragment");
  TEST_ASSERT_NOT_NULL(response);
  http_response_free(response);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_get_request_structure);
  RUN_TEST(test_post_request_structure);
  RUN_TEST(test_post_empty_body);
  RUN_TEST(test_custom_headers);
  RUN_TEST(test_request_with_body_and_headers);
  RUN_TEST(test_different_http_methods);
  RUN_TEST(test_url_with_query_params);
  RUN_TEST(test_url_with_fragment);

  return UNITY_END();
}
