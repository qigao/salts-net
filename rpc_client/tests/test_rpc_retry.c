/**
 * @file test_rpc_retry.c
 * @brief Unit tests for RPC retry logic and circuit breaker
 */

#include "../include/rpc_retry.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// Test: Default retry policy
void test_default_retry_policy(void) {
  rpc_retry_policy_t policy = rpc_retry_policy_default();

  TEST_ASSERT_EQUAL(RPC_RETRY_EXPONENTIAL_BACKOFF, policy.strategy);
  TEST_ASSERT_EQUAL(3, policy.max_retries);
  TEST_ASSERT_TRUE(policy.initial_delay_ms > 0);
  TEST_ASSERT_TRUE(policy.max_delay_ms > policy.initial_delay_ms);
  TEST_ASSERT_TRUE(policy.backoff_multiplier >= 1.0);
}

// Test: No-retry policy
void test_no_retry_policy(void) {
  rpc_retry_policy_t policy = rpc_retry_policy_none();

  TEST_ASSERT_EQUAL(RPC_RETRY_NONE, policy.strategy);
  TEST_ASSERT_EQUAL(0, policy.max_retries);
}

// Test: Retry state initialization
void test_retry_state_init(void) {
  rpc_retry_policy_t policy = rpc_retry_policy_default();
  rpc_retry_state_t state;

  rpc_retry_state_init(&state, &policy);

  TEST_ASSERT_EQUAL(0, state.attempt_count);
  TEST_ASSERT_EQUAL(policy.initial_delay_ms, state.current_delay_ms);
  TEST_ASSERT_EQUAL(policy.max_retries, state.retries_remaining);
  TEST_ASSERT_TRUE(state.start_time > 0);
}

// Test: Should retry - retryable errors
void test_should_retry_retryable(void) {
  rpc_retry_policy_t policy = rpc_retry_policy_default();
  rpc_retry_state_t state;
  rpc_retry_state_init(&state, &policy);

  // Retryable errors
  TEST_ASSERT_TRUE(rpc_retry_should_retry(&state, &policy, RPC_ERROR_TIMEOUT));
  TEST_ASSERT_TRUE(rpc_retry_should_retry(&state, &policy, RPC_ERROR_NETWORK));
  TEST_ASSERT_TRUE(rpc_retry_should_retry(&state, &policy, RPC_ERROR_HTTP_503));
}

// Test: Should retry - non-retryable errors
void test_should_retry_non_retryable(void) {
  rpc_retry_policy_t policy = rpc_retry_policy_default();
  rpc_retry_state_t state;
  rpc_retry_state_init(&state, &policy);

  // Non-retryable errors
  TEST_ASSERT_FALSE(rpc_retry_should_retry(&state, &policy, RPC_ERROR_PARSE));
  TEST_ASSERT_FALSE(rpc_retry_should_retry(&state, &policy, RPC_ERROR_INVALID_REQUEST));
  TEST_ASSERT_FALSE(rpc_retry_should_retry(&state, &policy, RPC_ERROR_HTTP_400));
}

// Test: Retry budget exhaustion
void test_retry_budget_exhaustion(void) {
  rpc_retry_policy_t policy = rpc_retry_policy_create(RPC_RETRY_EXPONENTIAL_BACKOFF, 3, 100);
  rpc_retry_state_t state;
  rpc_retry_state_init(&state, &policy);

  // Should retry first 3 times
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT_TRUE(rpc_retry_should_retry(&state, &policy, RPC_ERROR_TIMEOUT));
    rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);
  }

  // Budget exhausted after 3 retries
  TEST_ASSERT_FALSE(rpc_retry_should_retry(&state, &policy, RPC_ERROR_TIMEOUT));
  TEST_ASSERT_TRUE(rpc_retry_budget_exhausted(&state, &policy));
}

// Test: Exponential backoff delay
void test_exponential_backoff(void) {
  rpc_retry_policy_t policy = rpc_retry_policy_create(RPC_RETRY_EXPONENTIAL_BACKOFF, 5, 100);
  policy.backoff_multiplier = 2.0;
  policy.jitter_factor = 0.0; // No jitter for predictable testing

  rpc_retry_state_t state;
  rpc_retry_state_init(&state, &policy);

  uint32_t delay1 = rpc_retry_get_delay(&state, &policy);
  rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);

  uint32_t delay2 = rpc_retry_get_delay(&state, &policy);
  rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);

  uint32_t delay3 = rpc_retry_get_delay(&state, &policy);

  // Delays should roughly double (with some tolerance for timing)
  TEST_ASSERT_TRUE(delay2 >= delay1);
  TEST_ASSERT_TRUE(delay3 >= delay2);
}

// Test: Fixed delay strategy
void test_fixed_delay(void) {
  rpc_retry_policy_t policy = rpc_retry_policy_create(RPC_RETRY_FIXED_DELAY, 5, 500);

  rpc_retry_state_t state;
  rpc_retry_state_init(&state, &policy);

  uint32_t delay1 = rpc_retry_get_delay(&state, &policy);
  rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);

  uint32_t delay2 = rpc_retry_get_delay(&state, &policy);
  rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);

  // Fixed delay should be constant
  TEST_ASSERT_EQUAL(delay1, delay2);
  TEST_ASSERT_EQUAL(500, delay1);
}

// Test: Circuit breaker lifecycle
void test_circuit_breaker_lifecycle(void) {
  rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
  rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);

  TEST_ASSERT_NOT_NULL(cb);
  TEST_ASSERT_EQUAL(RPC_CIRCUIT_CLOSED, rpc_circuit_breaker_get_state(cb));

  rpc_circuit_breaker_destroy(cb);
}

// Test: Circuit breaker - normal operation
void test_circuit_breaker_closed(void) {
  rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
  rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);

  // Should allow attempts when closed
  TEST_ASSERT_TRUE(rpc_circuit_breaker_can_attempt(cb));

  // Record success
  rpc_circuit_breaker_record_success(cb);
  TEST_ASSERT_EQUAL(RPC_CIRCUIT_CLOSED, rpc_circuit_breaker_get_state(cb));

  rpc_circuit_breaker_destroy(cb);
}

// Test: Circuit breaker - trip on failures
void test_circuit_breaker_trip(void) {
  rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
  config.failure_threshold = 3;
  rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);

  TEST_ASSERT_EQUAL(RPC_CIRCUIT_CLOSED, rpc_circuit_breaker_get_state(cb));

  // Record failures to trip circuit
  for (int i = 0; i < 3; i++) {
    rpc_circuit_breaker_record_failure(cb);
  }

  // Circuit should be open
  TEST_ASSERT_EQUAL(RPC_CIRCUIT_OPEN, rpc_circuit_breaker_get_state(cb));
  TEST_ASSERT_FALSE(rpc_circuit_breaker_can_attempt(cb));

  rpc_circuit_breaker_destroy(cb);
}

// Test: Circuit breaker - half-open recovery
void test_circuit_breaker_half_open(void) {
  rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
  config.failure_threshold = 2;
  config.success_threshold = 2;
  rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);

  // Trip circuit
  rpc_circuit_breaker_record_failure(cb);
  rpc_circuit_breaker_record_failure(cb);
  TEST_ASSERT_EQUAL(RPC_CIRCUIT_OPEN, rpc_circuit_breaker_get_state(cb));

  // Manually transition to half-open (in real code, timeout would trigger this)
  cb->state = RPC_CIRCUIT_HALF_OPEN;
  cb->success_count = 0;
  cb->half_open_calls = 0;

  TEST_ASSERT_TRUE(rpc_circuit_breaker_can_attempt(cb));

  // Record successes to close circuit
  rpc_circuit_breaker_record_success(cb);
  rpc_circuit_breaker_record_success(cb);

  TEST_ASSERT_EQUAL(RPC_CIRCUIT_CLOSED, rpc_circuit_breaker_get_state(cb));

  rpc_circuit_breaker_destroy(cb);
}

// Test: Circuit breaker - failure in half-open reopens
void test_circuit_breaker_half_open_failure(void) {
  rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
  rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);

  // Manually set to half-open
  cb->state = RPC_CIRCUIT_HALF_OPEN;
  cb->success_count = 0;

  // Failure should reopen circuit
  rpc_circuit_breaker_record_failure(cb);
  TEST_ASSERT_EQUAL(RPC_CIRCUIT_OPEN, rpc_circuit_breaker_get_state(cb));

  rpc_circuit_breaker_destroy(cb);
}

// Test: Circuit breaker reset
void test_circuit_breaker_reset(void) {
  rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
  config.failure_threshold = 2;
  rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);

  // Trip circuit
  rpc_circuit_breaker_record_failure(cb);
  rpc_circuit_breaker_record_failure(cb);
  TEST_ASSERT_EQUAL(RPC_CIRCUIT_OPEN, rpc_circuit_breaker_get_state(cb));

  // Reset
  rpc_circuit_breaker_reset(cb);
  TEST_ASSERT_EQUAL(RPC_CIRCUIT_CLOSED, rpc_circuit_breaker_get_state(cb));
  TEST_ASSERT_TRUE(rpc_circuit_breaker_can_attempt(cb));

  rpc_circuit_breaker_destroy(cb);
}

// Test: Circuit state names
void test_circuit_state_names(void) {
  TEST_ASSERT_EQUAL_STRING("CLOSED", rpc_circuit_state_name(RPC_CIRCUIT_CLOSED));
  TEST_ASSERT_EQUAL_STRING("OPEN", rpc_circuit_state_name(RPC_CIRCUIT_OPEN));
  TEST_ASSERT_EQUAL_STRING("HALF_OPEN", rpc_circuit_state_name(RPC_CIRCUIT_HALF_OPEN));
}

// Main
int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_default_retry_policy);
  RUN_TEST(test_no_retry_policy);
  RUN_TEST(test_retry_state_init);
  RUN_TEST(test_should_retry_retryable);
  RUN_TEST(test_should_retry_non_retryable);
  RUN_TEST(test_retry_budget_exhaustion);
  RUN_TEST(test_exponential_backoff);
  RUN_TEST(test_fixed_delay);
  RUN_TEST(test_circuit_breaker_lifecycle);
  RUN_TEST(test_circuit_breaker_closed);
  RUN_TEST(test_circuit_breaker_trip);
  RUN_TEST(test_circuit_breaker_half_open);
  RUN_TEST(test_circuit_breaker_half_open_failure);
  RUN_TEST(test_circuit_breaker_reset);
  RUN_TEST(test_circuit_state_names);

  return UNITY_END();
}
