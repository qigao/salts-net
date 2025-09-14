#ifndef RPC_RETRY_H
#define RPC_RETRY_H

#include "rpc_error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file rpc_retry.h
 * @brief RPC Client Retry Logic with Exponential Backoff
 *
 * Provides sophisticated retry mechanisms with exponential backoff,
 * jitter, retry budgets, and circuit breaker pattern for production resilience.
 */

// ============================================================================
// Retry Policy
// ============================================================================

/**
 * Retry strategy
 */
typedef enum {
  RPC_RETRY_NONE = 0,              // No retries
  RPC_RETRY_IMMEDIATE,             // Retry immediately
  RPC_RETRY_FIXED_DELAY,           // Fixed delay between retries
  RPC_RETRY_EXPONENTIAL_BACKOFF,   // Exponential backoff with jitter
  RPC_RETRY_ADAPTIVE               // Adaptive based on server response
} rpc_retry_strategy_t;

/**
 * Retry policy configuration
 */
typedef struct {
  rpc_retry_strategy_t strategy;   // Retry strategy
  int max_retries;                 // Maximum number of retries
  uint32_t initial_delay_ms;       // Initial delay (milliseconds)
  uint32_t max_delay_ms;           // Maximum delay (milliseconds)
  double backoff_multiplier;       // Backoff multiplier (e.g., 2.0 for doubling)
  double jitter_factor;            // Jitter factor (0.0-1.0)
  uint32_t total_timeout_ms;       // Total timeout including all retries
  int retry_on_timeout;            // Retry on timeout errors
  int retry_on_network_error;      // Retry on network errors
  int retry_on_http_5xx;           // Retry on HTTP 5xx errors
} rpc_retry_policy_t;

/**
 * Retry state tracking
 */
typedef struct {
  int attempt_count;               // Current attempt number
  uint32_t current_delay_ms;       // Current delay
  uint64_t start_time;             // When retry sequence started
  uint64_t last_attempt_time;      // Last attempt timestamp
  rpc_error_code_t last_error;     // Last error encountered
  int retries_remaining;           // Retries remaining
} rpc_retry_state_t;

// ============================================================================
// Circuit Breaker
// ============================================================================

/**
 * Circuit breaker states
 */
typedef enum {
  RPC_CIRCUIT_CLOSED = 0,          // Normal operation
  RPC_CIRCUIT_OPEN,                // Failing, rejecting requests
  RPC_CIRCUIT_HALF_OPEN            // Testing if service recovered
} rpc_circuit_state_t;

/**
 * Circuit breaker configuration
 */
typedef struct {
  int failure_threshold;           // Failures before opening circuit
  int success_threshold;           // Successes to close circuit
  uint32_t timeout_ms;             // Time before trying half-open
  uint32_t half_open_max_calls;    // Max calls in half-open state
  double error_rate_threshold;     // Error rate to trip (0.0-1.0)
  int window_size;                 // Rolling window size for error rate
} rpc_circuit_breaker_config_t;

/**
 * Circuit breaker state
 */
typedef struct {
  rpc_circuit_state_t state;       // Current state
  int failure_count;               // Consecutive failures
  int success_count;               // Successes in half-open
  uint64_t last_failure_time;      // Last failure timestamp
  uint64_t open_time;              // When circuit opened
  int half_open_calls;             // Calls in half-open state

  // Rolling window for error rate
  int window[64];                  // 1=success, 0=failure
  int window_index;                // Current position
  int window_filled;               // Window is full

  rpc_circuit_breaker_config_t config;
} rpc_circuit_breaker_t;

// ============================================================================
// Retry Policy Functions
// ============================================================================

/**
 * Get default retry policy
 *
 * @return Default retry policy (exponential backoff)
 */
rpc_retry_policy_t rpc_retry_policy_default(void);

/**
 * Get no-retry policy
 *
 * @return Policy with no retries
 */
rpc_retry_policy_t rpc_retry_policy_none(void);

/**
 * Create custom retry policy
 *
 * @param strategy Retry strategy
 * @param max_retries Maximum retries
 * @param initial_delay_ms Initial delay
 * @return Retry policy
 */
rpc_retry_policy_t rpc_retry_policy_create(rpc_retry_strategy_t strategy, int max_retries,
                                             uint32_t initial_delay_ms);

/**
 * Initialize retry state from policy
 *
 * @param state Retry state (output)
 * @param policy Retry policy
 */
void rpc_retry_state_init(rpc_retry_state_t *state, const rpc_retry_policy_t *policy);

/**
 * Check if should retry
 *
 * @param state Retry state
 * @param policy Retry policy
 * @param error Last error
 * @return 1 if should retry, 0 otherwise
 */
int rpc_retry_should_retry(rpc_retry_state_t *state, const rpc_retry_policy_t *policy,
                            rpc_error_code_t error);

/**
 * Calculate next delay
 *
 * @param state Retry state
 * @param policy Retry policy
 * @return Delay in milliseconds
 */
uint32_t rpc_retry_get_delay(rpc_retry_state_t *state, const rpc_retry_policy_t *policy);

/**
 * Record retry attempt
 *
 * @param state Retry state
 * @param error Error that triggered retry
 */
void rpc_retry_record_attempt(rpc_retry_state_t *state, rpc_error_code_t error);

/**
 * Check if retry budget exhausted
 *
 * @param state Retry state
 * @param policy Retry policy
 * @return 1 if exhausted, 0 otherwise
 */
int rpc_retry_budget_exhausted(const rpc_retry_state_t *state,
                                 const rpc_retry_policy_t *policy);

// ============================================================================
// Circuit Breaker Functions
// ============================================================================

/**
 * Create circuit breaker
 *
 * @param config Circuit breaker configuration
 * @return Circuit breaker instance
 */
rpc_circuit_breaker_t *rpc_circuit_breaker_create(const rpc_circuit_breaker_config_t *config);

/**
 * Destroy circuit breaker
 *
 * @param cb Circuit breaker
 */
void rpc_circuit_breaker_destroy(rpc_circuit_breaker_t *cb);

/**
 * Get default circuit breaker configuration
 *
 * @return Default configuration
 */
rpc_circuit_breaker_config_t rpc_circuit_breaker_default_config(void);

/**
 * Check if can attempt request
 *
 * @param cb Circuit breaker
 * @return 1 if can attempt, 0 if circuit open
 */
int rpc_circuit_breaker_can_attempt(rpc_circuit_breaker_t *cb);

/**
 * Record successful request
 *
 * @param cb Circuit breaker
 */
void rpc_circuit_breaker_record_success(rpc_circuit_breaker_t *cb);

/**
 * Record failed request
 *
 * @param cb Circuit breaker
 */
void rpc_circuit_breaker_record_failure(rpc_circuit_breaker_t *cb);

/**
 * Get circuit state
 *
 * @param cb Circuit breaker
 * @return Current state
 */
rpc_circuit_state_t rpc_circuit_breaker_get_state(const rpc_circuit_breaker_t *cb);

/**
 * Reset circuit breaker
 *
 * @param cb Circuit breaker
 */
void rpc_circuit_breaker_reset(rpc_circuit_breaker_t *cb);

/**
 * Get circuit state name
 *
 * @param state Circuit state
 * @return State name string
 */
const char *rpc_circuit_state_name(rpc_circuit_state_t state);

#ifdef __cplusplus
}
#endif

#endif // RPC_RETRY_H
