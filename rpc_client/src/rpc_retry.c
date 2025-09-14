#include "../include/rpc_retry.h"
#include <platform.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// Helper Functions
// ============================================================================

static uint64_t get_timestamp_us(void) {
#ifdef _WIN32
  LARGE_INTEGER frequency, counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (uint64_t)((counter.QuadPart * 1000000ULL) / frequency.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)(ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000);
#endif
}

static double get_random_jitter(double jitter_factor) {
  if (jitter_factor <= 0.0)
    return 1.0;

  // Random value between (1 - jitter_factor) and (1 + jitter_factor)
  double random_val = (double)rand() / (double)RAND_MAX;
  return 1.0 + (random_val * 2.0 - 1.0) * jitter_factor;
}

// ============================================================================
// Retry Policy
// ============================================================================

rpc_retry_policy_t rpc_retry_policy_default(void) {
  rpc_retry_policy_t policy;
  memset(&policy, 0, sizeof(policy));

  policy.strategy = RPC_RETRY_EXPONENTIAL_BACKOFF;
  policy.max_retries = 3;
  policy.initial_delay_ms = 100;       // 100ms
  policy.max_delay_ms = 10000;         // 10 seconds
  policy.backoff_multiplier = 2.0;     // Double each time
  policy.jitter_factor = 0.1;          // 10% jitter
  policy.total_timeout_ms = 30000;     // 30 seconds total
  policy.retry_on_timeout = 1;
  policy.retry_on_network_error = 1;
  policy.retry_on_http_5xx = 1;

  return policy;
}

rpc_retry_policy_t rpc_retry_policy_none(void) {
  rpc_retry_policy_t policy;
  memset(&policy, 0, sizeof(policy));

  policy.strategy = RPC_RETRY_NONE;
  policy.max_retries = 0;

  return policy;
}

rpc_retry_policy_t rpc_retry_policy_create(rpc_retry_strategy_t strategy, int max_retries,
                                             uint32_t initial_delay_ms) {
  rpc_retry_policy_t policy;
  memset(&policy, 0, sizeof(policy));

  policy.strategy = strategy;
  policy.max_retries = max_retries;
  policy.initial_delay_ms = initial_delay_ms;
  policy.max_delay_ms = initial_delay_ms * 32; // Default max
  policy.backoff_multiplier = 2.0;
  policy.jitter_factor = 0.1;
  policy.total_timeout_ms = 30000;
  policy.retry_on_timeout = 1;
  policy.retry_on_network_error = 1;
  policy.retry_on_http_5xx = 1;

  return policy;
}

// ============================================================================
// Retry State
// ============================================================================

void rpc_retry_state_init(rpc_retry_state_t *state, const rpc_retry_policy_t *policy) {
  if (!state || !policy)
    return;

  memset(state, 0, sizeof(*state));
  state->start_time = get_timestamp_us();
  state->current_delay_ms = policy->initial_delay_ms;
  state->retries_remaining = policy->max_retries;
}

int rpc_retry_should_retry(rpc_retry_state_t *state, const rpc_retry_policy_t *policy,
                            rpc_error_code_t error) {
  if (!state || !policy)
    return 0;

  // No retries configured
  if (policy->strategy == RPC_RETRY_NONE || policy->max_retries == 0)
    return 0;

  // Exhausted retry budget
  if (state->attempt_count >= policy->max_retries)
    return 0;

  // Check total timeout
  if (policy->total_timeout_ms > 0) {
    uint64_t now = get_timestamp_us();
    uint64_t elapsed_ms = (now - state->start_time) / 1000;
    if (elapsed_ms >= policy->total_timeout_ms)
      return 0;
  }

  // Check if error is retryable based on policy
  int is_retryable = 0;

  // Timeout errors
  if (error == RPC_ERROR_TIMEOUT && policy->retry_on_timeout) {
    is_retryable = 1;
  }

  // Network errors
  if ((error == RPC_ERROR_NETWORK || error == RPC_ERROR_CONNECTION_LOST ||
       error == RPC_ERROR_CONNECT_FAILED || error == RPC_ERROR_DNS) &&
      policy->retry_on_network_error) {
    is_retryable = 1;
  }

  // HTTP 5xx errors
  if ((error == RPC_ERROR_HTTP_500 || error == RPC_ERROR_HTTP_502 ||
       error == RPC_ERROR_HTTP_503) &&
      policy->retry_on_http_5xx) {
    is_retryable = 1;
  }

  // Use generic retryable check from error module
  if (!is_retryable) {
    is_retryable = rpc_error_is_retryable(error);
  }

  return is_retryable;
}

uint32_t rpc_retry_get_delay(rpc_retry_state_t *state, const rpc_retry_policy_t *policy) {
  if (!state || !policy)
    return 0;

  uint32_t delay_ms = 0;

  switch (policy->strategy) {
  case RPC_RETRY_NONE:
    delay_ms = 0;
    break;

  case RPC_RETRY_IMMEDIATE:
    delay_ms = 0;
    break;

  case RPC_RETRY_FIXED_DELAY:
    delay_ms = policy->initial_delay_ms;
    break;

  case RPC_RETRY_EXPONENTIAL_BACKOFF: {
    // Calculate exponential backoff
    delay_ms = state->current_delay_ms;

    // Apply jitter
    if (policy->jitter_factor > 0.0) {
      double jitter = get_random_jitter(policy->jitter_factor);
      delay_ms = (uint32_t)((double)delay_ms * jitter);
    }

    // Cap at max delay
    if (policy->max_delay_ms > 0 && delay_ms > policy->max_delay_ms) {
      delay_ms = policy->max_delay_ms;
    }
    break;
  }

  case RPC_RETRY_ADAPTIVE:
    // For adaptive, could use server-provided Retry-After header
    // For now, use exponential backoff
    delay_ms = state->current_delay_ms;
    break;
  }

  return delay_ms;
}

void rpc_retry_record_attempt(rpc_retry_state_t *state, rpc_error_code_t error) {
  if (!state)
    return;

  state->attempt_count++;
  state->last_attempt_time = get_timestamp_us();
  state->last_error = error;

  // Update delay for next retry (exponential backoff)
  if (state->current_delay_ms > 0) {
    state->current_delay_ms = (uint32_t)((double)state->current_delay_ms * 2.0);
  }

  if (state->retries_remaining > 0) {
    state->retries_remaining--;
  }
}

int rpc_retry_budget_exhausted(const rpc_retry_state_t *state,
                                 const rpc_retry_policy_t *policy) {
  if (!state || !policy)
    return 1;

  // Check retry count
  if (state->attempt_count >= policy->max_retries)
    return 1;

  // Check total timeout
  if (policy->total_timeout_ms > 0) {
    uint64_t now = get_timestamp_us();
    uint64_t elapsed_ms = (now - state->start_time) / 1000;
    if (elapsed_ms >= policy->total_timeout_ms)
      return 1;
  }

  return 0;
}

// ============================================================================
// Circuit Breaker
// ============================================================================

rpc_circuit_breaker_config_t rpc_circuit_breaker_default_config(void) {
  rpc_circuit_breaker_config_t config;
  memset(&config, 0, sizeof(config));

  config.failure_threshold = 5;         // Open after 5 failures
  config.success_threshold = 2;         // Close after 2 successes in half-open
  config.timeout_ms = 60000;            // 60 seconds before trying half-open
  config.half_open_max_calls = 3;       // Allow 3 calls in half-open
  config.error_rate_threshold = 0.5;    // 50% error rate trips breaker
  config.window_size = 20;              // Look at last 20 requests

  return config;
}

rpc_circuit_breaker_t *rpc_circuit_breaker_create(const rpc_circuit_breaker_config_t *config) {
  rpc_circuit_breaker_t *cb = (rpc_circuit_breaker_t *)calloc(1, sizeof(rpc_circuit_breaker_t));
  if (!cb)
    return NULL;

  if (config) {
    cb->config = *config;
  } else {
    cb->config = rpc_circuit_breaker_default_config();
  }

  cb->state = RPC_CIRCUIT_CLOSED;
  return cb;
}

void rpc_circuit_breaker_destroy(rpc_circuit_breaker_t *cb) {
  if (cb)
    free(cb);
}

int rpc_circuit_breaker_can_attempt(rpc_circuit_breaker_t *cb) {
  if (!cb)
    return 1;

  uint64_t now = get_timestamp_us();

  switch (cb->state) {
  case RPC_CIRCUIT_CLOSED:
    // Normal operation
    return 1;

  case RPC_CIRCUIT_OPEN: {
    // Check if timeout elapsed
    uint64_t elapsed_ms = (now - cb->open_time) / 1000;
    if (elapsed_ms >= cb->config.timeout_ms) {
      // Transition to half-open
      cb->state = RPC_CIRCUIT_HALF_OPEN;
      cb->success_count = 0;
      cb->half_open_calls = 0;
      return 1;
    }
    return 0; // Circuit still open
  }

  case RPC_CIRCUIT_HALF_OPEN:
    // Allow limited calls in half-open state
    if (cb->half_open_calls < cb->config.half_open_max_calls) {
      return 1;
    }
    return 0;
  }

  return 0;
}

void rpc_circuit_breaker_record_success(rpc_circuit_breaker_t *cb) {
  if (!cb)
    return;

  // Update rolling window
  cb->window[cb->window_index] = 1; // Success
  cb->window_index = (cb->window_index + 1) % 64;
  if (cb->window_index == 0)
    cb->window_filled = 1;

  switch (cb->state) {
  case RPC_CIRCUIT_CLOSED:
    // Reset failure count on success
    cb->failure_count = 0;
    break;

  case RPC_CIRCUIT_HALF_OPEN:
    cb->success_count++;
    cb->half_open_calls++;

    // Check if enough successes to close circuit
    if (cb->success_count >= cb->config.success_threshold) {
      cb->state = RPC_CIRCUIT_CLOSED;
      cb->failure_count = 0;
      cb->success_count = 0;
    }
    break;

  case RPC_CIRCUIT_OPEN:
    // Shouldn't happen, but handle gracefully
    break;
  }
}

void rpc_circuit_breaker_record_failure(rpc_circuit_breaker_t *cb) {
  if (!cb)
    return;

  uint64_t now = get_timestamp_us();
  cb->last_failure_time = now;

  // Update rolling window
  cb->window[cb->window_index] = 0; // Failure
  cb->window_index = (cb->window_index + 1) % 64;
  if (cb->window_index == 0)
    cb->window_filled = 1;

  switch (cb->state) {
  case RPC_CIRCUIT_CLOSED:
    cb->failure_count++;

    // Check if should open circuit
    if (cb->failure_count >= cb->config.failure_threshold) {
      cb->state = RPC_CIRCUIT_OPEN;
      cb->open_time = now;
    }

    // Also check error rate in rolling window
    if (cb->window_filled) {
      int failures = 0;
      int window_size =
          cb->config.window_size < 64 ? cb->config.window_size : 64;
      for (int i = 0; i < window_size; i++) {
        if (cb->window[i] == 0)
          failures++;
      }
      double error_rate = (double)failures / (double)window_size;
      if (error_rate >= cb->config.error_rate_threshold) {
        cb->state = RPC_CIRCUIT_OPEN;
        cb->open_time = now;
      }
    }
    break;

  case RPC_CIRCUIT_HALF_OPEN:
    // Failure in half-open immediately reopens circuit
    cb->state = RPC_CIRCUIT_OPEN;
    cb->open_time = now;
    cb->success_count = 0;
    cb->half_open_calls = 0;
    break;

  case RPC_CIRCUIT_OPEN:
    // Already open, just track
    break;
  }
}

rpc_circuit_state_t rpc_circuit_breaker_get_state(const rpc_circuit_breaker_t *cb) {
  return cb ? cb->state : RPC_CIRCUIT_CLOSED;
}

void rpc_circuit_breaker_reset(rpc_circuit_breaker_t *cb) {
  if (!cb)
    return;

  cb->state = RPC_CIRCUIT_CLOSED;
  cb->failure_count = 0;
  cb->success_count = 0;
  cb->half_open_calls = 0;
  memset(cb->window, 0, sizeof(cb->window));
  cb->window_index = 0;
  cb->window_filled = 0;
}

const char *rpc_circuit_state_name(rpc_circuit_state_t state) {
  switch (state) {
  case RPC_CIRCUIT_CLOSED:
    return "CLOSED";
  case RPC_CIRCUIT_OPEN:
    return "OPEN";
  case RPC_CIRCUIT_HALF_OPEN:
    return "HALF_OPEN";
  default:
    return "UNKNOWN";
  }
}
