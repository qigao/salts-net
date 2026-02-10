#include "../include/rpc_retry.h"
#include "tinytest.h"
#include <string.h>

spec("rpc_retry") {
  describe("Default retry policy") {
    it("should have correct default settings") {
      rpc_retry_policy_t policy = rpc_retry_policy_default();

      check_int_eq(policy.strategy, RPC_RETRY_EXPONENTIAL_BACKOFF);
      check_int_eq(policy.max_retries, 3);
      check(policy.initial_delay_ms > 0);
      check(policy.max_delay_ms > policy.initial_delay_ms);
      check(policy.backoff_multiplier >= 1.0);
    }
  }

  describe("No-retry policy") {
    it("should have zero retries") {
      rpc_retry_policy_t policy = rpc_retry_policy_none();

      check_int_eq(policy.strategy, RPC_RETRY_NONE);
      check_int_eq(policy.max_retries, 0);
    }
  }

  describe("Retry state initialization") {
    it("should initialize state correctly") {
      rpc_retry_policy_t policy = rpc_retry_policy_default();
      rpc_retry_state_t state;

      rpc_retry_state_init(&state, &policy);

      check_int_eq(state.attempt_count, 0);
      check_int_eq(state.current_delay_ms, policy.initial_delay_ms);
      check_int_eq(state.retries_remaining, policy.max_retries);
      check(state.start_time > 0);
    }
  }

  describe("Should retry") {
    it("should correctly identify retryable errors") {
      rpc_retry_policy_t policy = rpc_retry_policy_default();
      rpc_retry_state_t state;
      rpc_retry_state_init(&state, &policy);

      // Retryable errors
      check(rpc_retry_should_retry(&state, &policy, RPC_ERROR_TIMEOUT));
      check(rpc_retry_should_retry(&state, &policy, RPC_ERROR_NETWORK));
      check(rpc_retry_should_retry(&state, &policy, RPC_ERROR_HTTP_503));
    }

    it("should correctly identify non-retryable errors") {
      rpc_retry_policy_t policy = rpc_retry_policy_default();
      rpc_retry_state_t state;
      rpc_retry_state_init(&state, &policy);

      // Non-retryable errors
      check(!rpc_retry_should_retry(&state, &policy, RPC_ERROR_PARSE));
      check(!rpc_retry_should_retry(&state, &policy, RPC_ERROR_INVALID_REQUEST));
      check(!rpc_retry_should_retry(&state, &policy, RPC_ERROR_HTTP_400));
    }
  }

  describe("Retry budget exhaustion") {
    it("should exhaust budget after max retries") {
      rpc_retry_policy_t policy = rpc_retry_policy_create(RPC_RETRY_EXPONENTIAL_BACKOFF, 3, 100);
      rpc_retry_state_t state;
      rpc_retry_state_init(&state, &policy);

      // Should retry first 3 times
      for (int i = 0; i < 3; i++) {
        check(rpc_retry_should_retry(&state, &policy, RPC_ERROR_TIMEOUT));
        rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);
      }

      // Budget exhausted after 3 retries
      check(!rpc_retry_should_retry(&state, &policy, RPC_ERROR_TIMEOUT));
      check(rpc_retry_budget_exhausted(&state, &policy));
    }
  }

  describe("Delay strategies") {
    it("should follow exponential backoff") {
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

      // Delays should roughly double
      check(delay2 >= delay1);
      check(delay3 >= delay2);
    }

    it("should follow fixed delay") {
      rpc_retry_policy_t policy = rpc_retry_policy_create(RPC_RETRY_FIXED_DELAY, 5, 500);

      rpc_retry_state_t state;
      rpc_retry_state_init(&state, &policy);

      uint32_t delay1 = rpc_retry_get_delay(&state, &policy);
      rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);

      uint32_t delay2 = rpc_retry_get_delay(&state, &policy);
      rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);

      // Fixed delay should be constant
      check_int_eq(delay1, delay2);
      check_int_eq(delay1, 500);
    }
  }

  describe("Circuit breaker") {
    it("should have correct lifecycle") {
      rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
      rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);

      check_not_null(cb);
      check_int_eq(rpc_circuit_breaker_get_state(cb), RPC_CIRCUIT_CLOSED);

      rpc_circuit_breaker_destroy(cb);
    }

    it("should operate normally when closed") {
      rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
      rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);

      // Should allow attempts when closed
      check(rpc_circuit_breaker_can_attempt(cb));

      // Record success
      rpc_circuit_breaker_record_success(cb);
      check_int_eq(rpc_circuit_breaker_get_state(cb), RPC_CIRCUIT_CLOSED);

      rpc_circuit_breaker_destroy(cb);
    }

    it("should trip on failures") {
      rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
      config.failure_threshold = 3;
      rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);

      check_int_eq(rpc_circuit_breaker_get_state(cb), RPC_CIRCUIT_CLOSED);

      // Record failures to trip circuit
      for (int i = 0; i < 3; i++) {
        rpc_circuit_breaker_record_failure(cb);
      }

      // Circuit should be open
      check_int_eq(rpc_circuit_breaker_get_state(cb), RPC_CIRCUIT_OPEN);
      check(!rpc_circuit_breaker_can_attempt(cb));

      rpc_circuit_breaker_destroy(cb);
    }

    it("should recover from half-open state") {
      rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
      config.failure_threshold = 2;
      config.success_threshold = 2;
      rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);

      // Trip circuit
      rpc_circuit_breaker_record_failure(cb);
      rpc_circuit_breaker_record_failure(cb);
      check_int_eq(rpc_circuit_breaker_get_state(cb), RPC_CIRCUIT_OPEN);

      // Manually transition to half-open (in real code, timeout would trigger this)
      cb->state = RPC_CIRCUIT_HALF_OPEN;
      cb->success_count = 0;
      cb->half_open_calls = 0;

      check(rpc_circuit_breaker_can_attempt(cb));

      // Record successes to close circuit
      rpc_circuit_breaker_record_success(cb);
      rpc_circuit_breaker_record_success(cb);

      check_int_eq(rpc_circuit_breaker_get_state(cb), RPC_CIRCUIT_CLOSED);

      rpc_circuit_breaker_destroy(cb);
    }

    it("should reopen if failure occurs in half-open state") {
      rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
      rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);

      // Manually set to half-open
      cb->state = RPC_CIRCUIT_HALF_OPEN;
      cb->success_count = 0;

      // Failure should reopen circuit
      rpc_circuit_breaker_record_failure(cb);
      check_int_eq(rpc_circuit_breaker_get_state(cb), RPC_CIRCUIT_OPEN);

      rpc_circuit_breaker_destroy(cb);
    }

    it("should reset correctly") {
      rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
      config.failure_threshold = 2;
      rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);

      // Trip circuit
      rpc_circuit_breaker_record_failure(cb);
      rpc_circuit_breaker_record_failure(cb);
      check_int_eq(rpc_circuit_breaker_get_state(cb), RPC_CIRCUIT_OPEN);

      // Reset
      rpc_circuit_breaker_reset(cb);
      check_int_eq(rpc_circuit_breaker_get_state(cb), RPC_CIRCUIT_CLOSED);
      check(rpc_circuit_breaker_can_attempt(cb));

      rpc_circuit_breaker_destroy(cb);
    }

    it("should return correct state names") {
      check_str_eq(rpc_circuit_state_name(RPC_CIRCUIT_CLOSED), "CLOSED");
      check_str_eq(rpc_circuit_state_name(RPC_CIRCUIT_OPEN), "OPEN");
      check_str_eq(rpc_circuit_state_name(RPC_CIRCUIT_HALF_OPEN), "HALF_OPEN");
    }
  }
}

