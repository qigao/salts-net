#include "../include/rpc_client.h"
#include "../include/rpc_error.h"
#include "../include/rpc_retry.h"
#include "../include/rpc_stats.h"
#include "tinytest.h"
#include <netcore/turbo_coro_context.h>
#include <turbo_coro.h>
#include <stdio.h>
#include <string.h>

/* Helper: run a test function inside a coroutine with a coro context */
typedef void (*coro_test_fn)(turbo_coro_context_t *ctx);

typedef struct {
    coro_test_fn fn;
    turbo_coro_context_t *ctx;
} coro_test_args_t;

static void coro_test_wrapper(turbo_coro_t *co, void *arg) {
    coro_test_args_t *a = (coro_test_args_t *)arg;
    a->fn(a->ctx);
}

static void run_in_coro(coro_test_fn fn) {
    turbo_coro_context_t *ctx = turbo_coro_context_create(NULL);
    coro_test_args_t args = { .fn = fn, .ctx = ctx };
    turbo_coro_t *co = turbo_coro_create(coro_test_wrapper, &args, NULL);
    turbo_coro_resume(co);
    turbo_coro_context_run(ctx, TURBO_RUN_DEFAULT);
    turbo_coro_destroy(co);
    turbo_coro_context_destroy(ctx);
}

/* ── Test bodies ────────────────────────────────────────────────── */

static int test_block_number_ok;
static void test_get_block_number(turbo_coro_context_t *ctx) {
    rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
    config.timeout_ms = 15000;
    config.coro_ctx = ctx;

    rpc_client_t *client = rpc_client_create(&config);
    test_block_number_ok = 0;
    if (!client) return;

    rpc_call_result_t result;
    int ret = rpc_client_call(client, "eth_blockNumber", "[]", &result);
    if (ret == 0 && result.http_status == 200 && result.success && result.result) {
        if (strncmp(result.result, "\"0x", 3) == 0 || strncmp(result.result, "0x", 2) == 0)
            test_block_number_ok = 1;
    }
    rpc_result_free(&result);
    rpc_client_destroy(client);
}

static int test_chain_id_ok;
static void test_get_chain_id(turbo_coro_context_t *ctx) {
    rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
    config.timeout_ms = 15000;
    config.coro_ctx = ctx;

    rpc_client_t *client = rpc_client_create(&config);
    test_chain_id_ok = 0;
    if (!client) return;

    rpc_call_result_t result;
    int ret = rpc_client_call(client, "eth_chainId", "[]", &result);
    if (ret == 0 && result.http_status == 200 && result.success && result.result) {
        if (strstr(result.result, "0x1") != NULL)
            test_chain_id_ok = 1;
    }
    rpc_result_free(&result);
    rpc_client_destroy(client);
}

static int test_gas_price_ok;
static void test_get_gas_price(turbo_coro_context_t *ctx) {
    rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
    config.timeout_ms = 15000;
    config.coro_ctx = ctx;

    rpc_client_t *client = rpc_client_create(&config);
    test_gas_price_ok = 0;
    if (!client) return;

    rpc_call_result_t result;
    int ret = rpc_client_call(client, "eth_gasPrice", "[]", &result);
    if (ret == 0 && result.http_status == 200 && result.success && result.result)
        test_gas_price_ok = 1;
    rpc_result_free(&result);
    rpc_client_destroy(client);
}

static int test_invalid_method_ok;
static void test_invalid_method(turbo_coro_context_t *ctx) {
    rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
    config.timeout_ms = 15000;
    config.coro_ctx = ctx;

    rpc_client_t *client = rpc_client_create(&config);
    test_invalid_method_ok = 0;
    if (!client) return;

    rpc_call_result_t result;
    int ret = rpc_client_call(client, "invalid_method_that_does_not_exist", "[]", &result);
    if (ret == 0 && result.http_status == 200 && !result.success && result.error_message)
        test_invalid_method_ok = 1;
    rpc_result_free(&result);
    rpc_client_destroy(client);
}

static int test_stats_ok;
static void test_stats_with_requests(turbo_coro_context_t *ctx) {
    rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
    config.timeout_ms = 15000;
    config.coro_ctx = ctx;

    rpc_client_t *client = rpc_client_create(&config);
    test_stats_ok = 0;
    if (!client) return;

    rpc_stats_t *stats = rpc_stats_create();
    if (!stats) { rpc_client_destroy(client); return; }

    const char *methods[] = {"eth_blockNumber", "eth_chainId", "eth_gasPrice"};

    for (int i = 0; i < 3; i++) {
        uint64_t req_id = rpc_stats_request_start(stats);
        rpc_call_result_t result;
        int ret = rpc_client_call(client, methods[i], "[]", &result);
        if (ret == 0 && result.http_status == 200 && result.success) {
            rpc_stats_request_success(stats, req_id, 100, result.result ? strlen(result.result) : 0);
        } else {
            rpc_stats_request_failure(stats, req_id, RPC_ERROR_NETWORK);
        }
        rpc_result_free(&result);
    }

    if (stats->total_requests > 0 && rpc_stats_get_success_rate(stats) >= 0.0)
        test_stats_ok = 1;

    rpc_stats_destroy(stats);
    rpc_client_destroy(client);
}

/* ── Specs ──────────────────────────────────────────────────────── */

spec("eth_rpc_integration") {
  describe("Ethereum JSON-RPC") {
    it("should get current block number") {
      run_in_coro(test_get_block_number);
      check(test_block_number_ok);
    }

    it("should get chain ID (mainnet=0x1)") {
      run_in_coro(test_get_chain_id);
      check(test_chain_id_ok);
    }

    it("should get gas price") {
      run_in_coro(test_get_gas_price);
      check(test_gas_price_ok);
    }

    it("should handle invalid methods gracefully") {
      run_in_coro(test_invalid_method);
      check(test_invalid_method_ok);
    }
  }

  describe("System Metrics Integration") {
    it("should track stats with real requests") {
      run_in_coro(test_stats_with_requests);
      check(test_stats_ok);
    }
  }

  describe("Resilience Logic") {
    it("should verify retry configuration") {
      rpc_retry_policy_t policy = rpc_retry_policy_create(RPC_RETRY_EXPONENTIAL_BACKOFF, 3, 100);
      policy.retry_on_http_5xx = 1;

      rpc_retry_state_t state;
      rpc_retry_state_init(&state, &policy);

      check(rpc_retry_should_retry(&state, &policy, RPC_ERROR_TIMEOUT));
      check(!rpc_retry_budget_exhausted(&state, &policy));

      for (int i = 0; i < 3; i++) {
        rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);
      }

      check(!rpc_retry_should_retry(&state, &policy, RPC_ERROR_TIMEOUT));
      check(rpc_retry_budget_exhausted(&state, &policy));
    }

    it("should verify circuit breaker logic") {
      rpc_circuit_breaker_config_t config = rpc_circuit_breaker_default_config();
      config.failure_threshold = 3;

      rpc_circuit_breaker_t *cb = rpc_circuit_breaker_create(&config);
      check_not_null(cb);

      check_int_eq(rpc_circuit_breaker_get_state(cb), RPC_CIRCUIT_CLOSED);
      check(rpc_circuit_breaker_can_attempt(cb));

      for (int i = 0; i < 3; i++) {
        rpc_circuit_breaker_record_failure(cb);
      }

      check_int_eq(rpc_circuit_breaker_get_state(cb), RPC_CIRCUIT_OPEN);
      check(!rpc_circuit_breaker_can_attempt(cb));

      rpc_circuit_breaker_destroy(cb);
    }
  }
}
