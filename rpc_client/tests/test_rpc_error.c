/**
 * @file test_rpc_error.c
 * @brief Unit tests for RPC error handling
 */

#include "../include/rpc_error.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// Test: Error name retrieval
void test_error_name(void) {
  TEST_ASSERT_EQUAL_STRING("RPC_OK", rpc_error_name(RPC_OK));
  TEST_ASSERT_EQUAL_STRING("RPC_ERROR_TIMEOUT", rpc_error_name(RPC_ERROR_TIMEOUT));
  TEST_ASSERT_EQUAL_STRING("RPC_ERROR_PARSE", rpc_error_name(RPC_ERROR_PARSE));
  TEST_ASSERT_EQUAL_STRING("RPC_ERROR_HTTP_500", rpc_error_name(RPC_ERROR_HTTP_500));
  TEST_ASSERT_EQUAL_STRING("RPC_ERROR_UNKNOWN", rpc_error_name(99999));
}

// Test: Error description
void test_error_description(void) {
  const char *desc = rpc_error_description(RPC_ERROR_TIMEOUT);
  TEST_ASSERT_NOT_NULL(desc);
  TEST_ASSERT_TRUE(strlen(desc) > 0);

  desc = rpc_error_description(RPC_ERROR_PARSE);
  TEST_ASSERT_NOT_NULL(desc);
  TEST_ASSERT_TRUE(strstr(desc, "JSON") != NULL);
}

// Test: Error severity
void test_error_severity(void) {
  TEST_ASSERT_EQUAL(RPC_SEVERITY_ERROR, rpc_error_get_severity(RPC_ERROR_TIMEOUT));
  TEST_ASSERT_EQUAL(RPC_SEVERITY_FATAL, rpc_error_get_severity(RPC_ERROR_OUT_OF_MEMORY));
  TEST_ASSERT_EQUAL(RPC_SEVERITY_WARNING, rpc_error_get_severity(RPC_ERROR_CIRCUIT_OPEN));
}

// Test: Retryable errors
void test_retryable_errors(void) {
  // Retryable errors
  TEST_ASSERT_TRUE(rpc_error_is_retryable(RPC_ERROR_TIMEOUT));
  TEST_ASSERT_TRUE(rpc_error_is_retryable(RPC_ERROR_NETWORK));
  TEST_ASSERT_TRUE(rpc_error_is_retryable(RPC_ERROR_HTTP_503));
  TEST_ASSERT_TRUE(rpc_error_is_retryable(RPC_ERROR_HTTP_502));

  // Non-retryable errors
  TEST_ASSERT_FALSE(rpc_error_is_retryable(RPC_ERROR_PARSE));
  TEST_ASSERT_FALSE(rpc_error_is_retryable(RPC_ERROR_INVALID_REQUEST));
  TEST_ASSERT_FALSE(rpc_error_is_retryable(RPC_ERROR_HTTP_400));
  TEST_ASSERT_FALSE(rpc_error_is_retryable(RPC_ERROR_HTTP_404));
}

// Test: Fatal errors
void test_fatal_errors(void) {
  TEST_ASSERT_TRUE(rpc_error_is_fatal(RPC_ERROR_OUT_OF_MEMORY));
  TEST_ASSERT_TRUE(rpc_error_is_fatal(RPC_ERROR_BUFFER_OVERFLOW));
  TEST_ASSERT_FALSE(rpc_error_is_fatal(RPC_ERROR_TIMEOUT));
  TEST_ASSERT_FALSE(rpc_error_is_fatal(RPC_ERROR_NETWORK));
}

// Test: Error creation
void test_error_create(void) {
  rpc_error_info_t error = rpc_error_create(RPC_ERROR_TIMEOUT, "Request timeout", "Timeout after 5000ms");

  TEST_ASSERT_EQUAL(RPC_ERROR_TIMEOUT, error.code);
  TEST_ASSERT_EQUAL_STRING("Request timeout", error.message);
  TEST_ASSERT_EQUAL_STRING("Timeout after 5000ms", error.details);
  TEST_ASSERT_TRUE(error.timestamp > 0);
}

// Test: Error creation with defaults
void test_error_create_defaults(void) {
  rpc_error_info_t error = rpc_error_create(RPC_ERROR_NETWORK, NULL, NULL);

  TEST_ASSERT_EQUAL(RPC_ERROR_NETWORK, error.code);
  TEST_ASSERT_EQUAL_STRING("Network I/O error", error.message);
  TEST_ASSERT_TRUE(error.timestamp > 0);
}

// Test: Error formatting
void test_error_format(void) {
  rpc_error_info_t error = rpc_error_create(RPC_ERROR_TIMEOUT, "Request timeout", "Server not responding");
  error.http_status = 503;
  error.retry_count = 3;

  char buffer[512];
  size_t len = rpc_error_format(&error, buffer, sizeof(buffer));

  TEST_ASSERT_TRUE(len > 0);
  TEST_ASSERT_TRUE(strstr(buffer, "RPC_ERROR_TIMEOUT") != NULL);
  TEST_ASSERT_TRUE(strstr(buffer, "Request timeout") != NULL);
  TEST_ASSERT_TRUE(strstr(buffer, "503") != NULL);
  TEST_ASSERT_TRUE(strstr(buffer, "retries: 3") != NULL);
}

// Test: HTTP to RPC error mapping
void test_http_to_rpc_error(void) {
  TEST_ASSERT_EQUAL(RPC_ERROR_HTTP_400, rpc_error_from_http_status(400));
  TEST_ASSERT_EQUAL(RPC_ERROR_HTTP_401, rpc_error_from_http_status(401));
  TEST_ASSERT_EQUAL(RPC_ERROR_HTTP_404, rpc_error_from_http_status(404));
  TEST_ASSERT_EQUAL(RPC_ERROR_HTTP_500, rpc_error_from_http_status(500));
  TEST_ASSERT_EQUAL(RPC_ERROR_HTTP_503, rpc_error_from_http_status(503));
  TEST_ASSERT_EQUAL(RPC_ERROR_HTTP_OTHER, rpc_error_from_http_status(418)); // I'm a teapot
  TEST_ASSERT_EQUAL(RPC_OK, rpc_error_from_http_status(200));
}

// Main
int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_error_name);
  RUN_TEST(test_error_description);
  RUN_TEST(test_error_severity);
  RUN_TEST(test_retryable_errors);
  RUN_TEST(test_fatal_errors);
  RUN_TEST(test_error_create);
  RUN_TEST(test_error_create_defaults);
  RUN_TEST(test_error_format);
  RUN_TEST(test_http_to_rpc_error);

  return UNITY_END();
}
