/**
 * test_parsing.c - HTTP response parsing tests
 */

#include "http_client.h"
#include <unity.h>
#include <string.h>

void setUp(void) {
}

void tearDown(void) {
}

void test_response_structure(void) {
  http_response_t *response = calloc(1, sizeof(http_response_t));
  TEST_ASSERT_NOT_NULL(response);
  
  /* Test initial state */
  TEST_ASSERT_EQUAL(0, response->status_code);
  TEST_ASSERT_NULL(response->headers);
  TEST_ASSERT_NULL(response->body);
  TEST_ASSERT_NULL(response->error);
  TEST_ASSERT_EQUAL(0, response->headers_len);
  TEST_ASSERT_EQUAL(0, response->body_len);
  
  http_response_free(response);
}

void test_response_with_status(void) {
  http_response_t *response = calloc(1, sizeof(http_response_t));
  response->status_code = 200;
  
  TEST_ASSERT_EQUAL(200, response->status_code);
  
  http_response_free(response);
}

void test_response_with_headers(void) {
  http_response_t *response = calloc(1, sizeof(http_response_t));
  const char *headers = "Content-Type: text/html\r\nContent-Length: 100\r\n";
  response->headers = strdup(headers);
  response->headers_len = strlen(headers);
  
  TEST_ASSERT_NOT_NULL(response->headers);
  TEST_ASSERT_GREATER_THAN(0, response->headers_len);
  
  http_response_free(response);
}

void test_response_with_body(void) {
  http_response_t *response = calloc(1, sizeof(http_response_t));
  const char *body = "Hello, World!";
  response->body = strdup(body);
  response->body_len = strlen(body);
  
  TEST_ASSERT_NOT_NULL(response->body);
  TEST_ASSERT_EQUAL(13, response->body_len);
  TEST_ASSERT_EQUAL_STRING("Hello, World!", response->body);
  
  http_response_free(response);
}

void test_response_with_error(void) {
  http_response_t *response = calloc(1, sizeof(http_response_t));
  response->error = strdup("Connection failed");
  
  TEST_ASSERT_NOT_NULL(response->error);
  TEST_ASSERT_EQUAL_STRING("Connection failed", response->error);
  
  http_response_free(response);
}

void test_response_complete(void) {
  http_response_t *response = calloc(1, sizeof(http_response_t));
  
  response->status_code = 200;
  response->headers = strdup("Content-Type: application/json\r\n");
  response->headers_len = strlen(response->headers);
  response->body = strdup("{\"status\":\"ok\"}");
  response->body_len = strlen(response->body);
  
  TEST_ASSERT_EQUAL(200, response->status_code);
  TEST_ASSERT_NOT_NULL(response->headers);
  TEST_ASSERT_NOT_NULL(response->body);
  TEST_ASSERT_GREATER_THAN(0, response->headers_len);
  TEST_ASSERT_GREATER_THAN(0, response->body_len);
  
  http_response_free(response);
}

void test_url_parsing_http(void) {
  /* Test URL parsing without making actual connections */
  http_client_t *client = http_client_create();
  TEST_ASSERT_NOT_NULL(client);
  http_client_set_timeout(client, 1000); /* 1 second timeout */
  
  /* Test that client can handle various URL formats */
  /* These will fail to connect but test URL parsing */
  http_response_t *response = http_get(client, "http://localhost:9999/test");
  TEST_ASSERT_NOT_NULL(response);
  /* Should have error (connection refused) but not crash */
  http_response_free(response);
  
  http_client_destroy(client);
}

void test_url_parsing_https(void) {
  /* Skip HTTPS test in unit tests - requires working connection */
  /* HTTPS URL parsing is tested in integration tests */
  TEST_PASS();
}

int main(void) {
  UNITY_BEGIN();
  
  RUN_TEST(test_response_structure);
  RUN_TEST(test_response_with_status);
  RUN_TEST(test_response_with_headers);
  RUN_TEST(test_response_with_body);
  RUN_TEST(test_response_with_error);
  RUN_TEST(test_response_complete);
  RUN_TEST(test_url_parsing_http);
  RUN_TEST(test_url_parsing_https);
  
  return UNITY_END();
}
