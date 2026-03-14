#include "../include/rpc_client.h"
#include "../include/rpc_error.h"
#include "../include/rpc_retry.h"
#include "../include/rpc_stats.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>
#include <http_client.h>

spec("eth_rpc_integration") {
  describe("Ethereum JSON-RPC") {
    it("should get current block number") {
      http_client_t *http = http_client_create(NULL);
      http_client_set_timeout(http, 15000);
      rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
      config.http_client = http;
      rpc_client_t *client = rpc_client_create(&config);
      check_not_null(client);

      rpc_call_result_t result;
      int ret = rpc_client_call(client, "eth_blockNumber", "[]", &result);
      if (ret == 0 && result.http_status == 200 && result.success && result.result)
        check(strncmp(result.result, "\"0x", 3) == 0 || strncmp(result.result, "0x", 2) == 0);
      rpc_result_free(&result);
      rpc_client_destroy(client);
      http_client_destroy(http);
    }

    it("should get chain ID (mainnet=0x1)") {
      http_client_t *http = http_client_create(NULL);
      http_client_set_timeout(http, 15000);
      rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
      config.http_client = http;
      rpc_client_t *client = rpc_client_create(&config);
      check_not_null(client);

      rpc_call_result_t result;
      int ret = rpc_client_call(client, "eth_chainId", "[]", &result);
      if (ret == 0 && result.http_status == 200 && result.success && result.result)
        check(strstr(result.result, "0x1") != NULL);
      rpc_result_free(&result);
      rpc_client_destroy(client);
      http_client_destroy(http);
    }

    it("should get gas price") {
      http_client_t *http = http_client_create(NULL);
      http_client_set_timeout(http, 15000);
      rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
      config.http_client = http;
      rpc_client_t *client = rpc_client_create(&config);
      check_not_null(client);

      rpc_call_result_t result;
      int ret = rpc_client_call(client, "eth_gasPrice", "[]", &result);
      if (ret == 0 && result.http_status == 200 && result.success)
        check_not_null(result.result);
      rpc_result_free(&result);
      rpc_client_destroy(client);
      http_client_destroy(http);
    }

    it("should handle invalid methods gracefully") {
      http_client_t *http = http_client_create(NULL);
      http_client_set_timeout(http, 15000);
      rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
      config.http_client = http;
      rpc_client_t *client = rpc_client_create(&config);
      check_not_null(client);

      rpc_call_result_t result;
      int ret = rpc_client_call(client, "invalid_method_that_does_not_exist", "[]", &result);
      if (ret == 0 && result.http_status == 200)
        check(!result.success && result.error_message != NULL);
      rpc_result_free(&result);
      rpc_client_destroy(client);
      http_client_destroy(http);
    }
  }

  describe("System Metrics Integration") {
    it("should track stats with real requests") {
      http_client_t *http = http_client_create(NULL);
      http_client_set_timeout(http, 15000);
      rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
      config.http_client = http;
      rpc_client_t *client = rpc_client_create(&config);
      check_not_null(client);

      rpc_stats_t *stats = rpc_stats_create();
      check_not_null(stats);

      const char *methods[] = {"eth_blockNumber", "eth_chainId", "eth_gasPrice"};
      for (int i = 0; i < 3; i++) {
        uint64_t req_id = rpc_stats_request_start(stats);
        rpc_call_result_t result;
        int ret = rpc_client_call(client, methods[i], "[]", &result);
        if (ret == 0 && result.http_status == 200 && result.success)
          rpc_stats_request_success(stats, req_id, 100, result.result ? strlen(result.result) : 0);
        else
          rpc_stats_request_failure(stats, req_id, RPC_ERROR_NETWORK);
        rpc_result_free(&result);
      }

      check(stats->total_requests > 0);
      check(rpc_stats_get_success_rate(stats) >= 0.0);

      rpc_stats_destroy(stats);
      rpc_client_destroy(client);
      http_client_destroy(http);
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

      for (int i = 0; i < 3; i++)
        rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);

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

      for (int i = 0; i < 3; i++)
        rpc_circuit_breaker_record_failure(cb);

      check_int_eq(rpc_circuit_breaker_get_state(cb), RPC_CIRCUIT_OPEN);
      check(!rpc_circuit_breaker_can_attempt(cb));

      rpc_circuit_breaker_destroy(cb);
    }
  }
}
