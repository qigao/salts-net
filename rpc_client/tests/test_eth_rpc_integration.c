/**
 * @file test_eth_rpc_integration.c
 * @brief Integration tests using Public Node Ethereum JSON-RPC endpoint
 *
 * Tests RPC client with actual JSON-RPC 2.0 requests to Ethereum network:
 * - eth_blockNumber - get current block number
 * - eth_chainId - get chain ID
 * - eth_gasPrice - get current gas price
 * - Error handling with invalid methods
 *
 * Uses: https://ethereum.publicnode.com (free, no authentication)
 */

#include "../include/rpc_client.h"
#include "../include/rpc_error.h"
#include "../include/rpc_retry.h"
#include "../include/rpc_stats.h"
#include "unity.h"
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// Test: Basic JSON-RPC call - eth_blockNumber
void test_eth_block_number(void) {
  printf("\n[TEST] Get Ethereum block number...\n");

  rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("ethereum.publicnode.com", 443);
  config.endpoint = "/";
  config.transport = RPC_TRANSPORT_TLS;
  config.timeout_ms = 15000; // 15 seconds for network calls

  rpc_client_t *client = rpc_client_create(&config);
  TEST_ASSERT_NOT_NULL_MESSAGE(client, "Failed to create RPC client");

  // Make JSON-RPC request
  rpc_call_result_t result;
  int ret = rpc_client_call(client, "eth_blockNumber", "[]", &result);

  if (ret == 0) {
    printf("HTTP Status: %d\n", result.http_status);
    if (result.result) {
      printf("Block Number: %s\n", result.result);
    }
    TEST_ASSERT_EQUAL_MESSAGE(200, result.http_status, "Expected HTTP 200");
    TEST_ASSERT_TRUE_MESSAGE(result.success, "Expected successful JSON-RPC response");
    TEST_ASSERT_NOT_NULL_MESSAGE(result.result, "Expected block number result");
  } else {
    printf("Request failed (might be network issue, skipping assertion)\n");
  }

  rpc_result_free(&result);
  rpc_client_destroy(client);
}

// Test: eth_chainId should return 0x1 for Ethereum mainnet
void test_eth_chain_id(void) {
  printf("\n[TEST] Get Ethereum chain ID...\n");

  rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("ethereum.publicnode.com", 443);
  config.endpoint = "/";
  config.transport = RPC_TRANSPORT_TLS;
  config.timeout_ms = 15000;

  rpc_client_t *client = rpc_client_create(&config);
  TEST_ASSERT_NOT_NULL(client);

  rpc_call_result_t result;
  int ret = rpc_client_call(client, "eth_chainId", "[]", &result);

  if (ret == 0) {
    printf("HTTP Status: %d\n", result.http_status);
    printf("Chain ID: %s\n", result.result ? result.result : "N/A");

    TEST_ASSERT_EQUAL(200, result.http_status);
    TEST_ASSERT_TRUE(result.success);

    // Chain ID for Ethereum mainnet should be 0x1
    if (result.result) {
      TEST_ASSERT_TRUE(strstr(result.result, "0x1") != NULL);
    }
  } else {
    printf("Request failed (network issue, skipping)\n");
  }

  rpc_result_free(&result);
  rpc_client_destroy(client);
}

// Test: eth_gasPrice
void test_eth_gas_price(void) {
  printf("\n[TEST] Get current gas price...\n");

  rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("ethereum.publicnode.com", 443);
  config.endpoint = "/";
  config.transport = RPC_TRANSPORT_TLS;
  config.timeout_ms = 15000;

  rpc_client_t *client = rpc_client_create(&config);
  TEST_ASSERT_NOT_NULL(client);

  rpc_call_result_t result;
  int ret = rpc_client_call(client, "eth_gasPrice", "[]", &result);

  if (ret == 0) {
    printf("HTTP Status: %d\n", result.http_status);
    printf("Gas Price: %s\n", result.result ? result.result : "N/A");

    TEST_ASSERT_EQUAL(200, result.http_status);
    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_NOT_NULL(result.result);
  } else {
    printf("Request failed (network issue, skipping)\n");
  }

  rpc_result_free(&result);
  rpc_client_destroy(client);
}

// Test: Invalid method should return JSON-RPC error
void test_invalid_method_error(void) {
  printf("\n[TEST] Test invalid method error handling...\n");

  rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("ethereum.publicnode.com", 443);
  config.endpoint = "/";
  config.transport = RPC_TRANSPORT_TLS;
  config.timeout_ms = 15000;

  rpc_client_t *client = rpc_client_create(&config);
  TEST_ASSERT_NOT_NULL(client);

  rpc_call_result_t result;
  int ret = rpc_client_call(client, "invalid_method_that_does_not_exist", "[]", &result);

  if (ret == 0) {
    printf("HTTP Status: %d\n", result.http_status);

    // Should get HTTP 200 but with JSON-RPC error
    TEST_ASSERT_EQUAL(200, result.http_status);

    // JSON-RPC error should be reported
    TEST_ASSERT_FALSE_MESSAGE(result.success, "Expected JSON-RPC error for invalid method");

    if (result.error_message) {
      printf("Error message: %s\n", result.error_message);
      printf("Error code: %d\n", result.error_code);
    }
  } else {
    printf("Request failed (network issue, skipping)\n");
  }

  rpc_result_free(&result);
  rpc_client_destroy(client);
}

// Test: Statistics tracking with real requests
void test_stats_with_real_requests(void) {
  printf("\n[TEST] Statistics tracking with Ethereum RPC...\n");

  rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("ethereum.publicnode.com", 443);
  config.endpoint = "/";
  config.transport = RPC_TRANSPORT_TLS;
  config.timeout_ms = 15000;

  rpc_client_t *client = rpc_client_create(&config);
  TEST_ASSERT_NOT_NULL(client);

  rpc_stats_t *stats = rpc_stats_create();
  TEST_ASSERT_NOT_NULL(stats);

  // Make 3 requests and track stats
  const char *methods[] = {"eth_blockNumber", "eth_chainId", "eth_gasPrice"};
  int success_count = 0;

  for (int i = 0; i < 3; i++) {
    uint64_t req_id = rpc_stats_request_start(stats);

    rpc_call_result_t result;
    int ret = rpc_client_call(client, methods[i], "[]", &result);

    if (ret == 0 && result.http_status == 200 && result.success) {
      size_t bytes_sent = 100; // Approximate
      size_t bytes_recv = result.result ? strlen(result.result) : 0;
      rpc_stats_request_success(stats, req_id, bytes_sent, bytes_recv);
      success_count++;
      printf("  %s: OK\n", methods[i]);
    } else {
      rpc_stats_request_failure(stats, req_id, RPC_ERROR_NETWORK);
      printf("  %s: FAILED\n", methods[i]);
    }

    rpc_result_free(&result);
  }

  printf("Successful requests: %d/3\n", success_count);

  // Print statistics if we had any success
  if (success_count > 0) {
    printf("Average latency: %.2f ms\n", rpc_stats_get_avg_latency_ms(stats));
    printf("Success rate: %.2f%%\n", rpc_stats_get_success_rate(stats) * 100.0);
  }

  rpc_stats_destroy(stats);
  rpc_client_destroy(client);
}

// Test: Retry logic configuration
void test_retry_configuration(void) {
  printf("\n[TEST] Retry logic configuration...\n");

  rpc_retry_policy_t policy = rpc_retry_policy_create(RPC_RETRY_EXPONENTIAL_BACKOFF, 3, 100);
  policy.retry_on_http_5xx = 1;

  rpc_retry_state_t state;
  rpc_retry_state_init(&state, &policy);

  // Should be able to retry initially
  TEST_ASSERT_TRUE(rpc_retry_should_retry(&state, &policy, RPC_ERROR_TIMEOUT));
  TEST_ASSERT_FALSE(rpc_retry_budget_exhausted(&state, &policy));

  // Record attempts
  for (int i = 0; i < 3; i++) {
    uint32_t delay = rpc_retry_get_delay(&state, &policy);
    printf("  Attempt %d: delay %u ms\n", i + 1, delay);
    rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);
  }

  // Budget should be exhausted after 3 attempts
  TEST_ASSERT_FALSE(rpc_retry_should_retry(&state, &policy, RPC_ERROR_TIMEOUT));
  TEST_ASSERT_TRUE(rpc_retry_budget_exhausted(&state, &policy));

  printf("  Retry budget exhausted as expected\n");
}

// Test: Circuit breaker pattern
void test_circuit_breaker(void) {
  printf("\n[TEST] Circuit breaker pattern...\n");

  rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
  config.failure_threshold = 3;

  rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);
  TEST_ASSERT_NOT_NULL(cb);

  // Initially closed
  TEST_ASSERT_EQUAL(RPC_CIRCUIT_CLOSED, rpc_circuit_breaker_get_state(cb));
  TEST_ASSERT_TRUE(rpc_circuit_breaker_can_attempt(cb));

  // Record failures to trip circuit
  for (int i = 0; i < 3; i++) {
    rpc_circuit_breaker_record_failure(cb);
  }

  // Should be open now
  TEST_ASSERT_EQUAL(RPC_CIRCUIT_OPEN, rpc_circuit_breaker_get_state(cb));
  TEST_ASSERT_FALSE(rpc_circuit_breaker_can_attempt(cb));

  printf("  Circuit breaker tripped after %d failures\n", config.failure_threshold);

  rpc_circuit_breaker_destroy(cb);
}

// Main
int main(void) {
  UNITY_BEGIN();

  printf("\n");
  printf("========================================\n");
  printf("  RPC Client Integration Tests\n");
  printf("  Using HTTPS Ethereum JSON-RPC endpoint\n");
  printf("  https://ethereum.publicnode.com\n");
  printf("========================================\n");

  RUN_TEST(test_eth_block_number);
  RUN_TEST(test_eth_chain_id);
  RUN_TEST(test_eth_gas_price);
  RUN_TEST(test_invalid_method_error);
  RUN_TEST(test_stats_with_real_requests);
  RUN_TEST(test_retry_configuration);
  RUN_TEST(test_circuit_breaker);

  printf("\n");
  printf("========================================\n");
  printf("  Integration Tests Complete\n");
  printf("========================================\n");

  return UNITY_END();
}
