
#include "tinytest.h"
#include "rpc_client.h"
#include "rpc_error.h"
#include "rpc_retry.h"
#include "rpc_stats.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

spec("Ethereum JSON-RPC Examples (BDD)") {
    static rpc_client_t *client = NULL;
    static rpc_client_config_t config;

    before_each() {
        config = (rpc_client_config_t)RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
        config.timeout_ms = 10000; // 10 seconds
        client = rpc_client_create(&config);
        check(client != NULL);
    }

    after_each() {
        if (client) {
            rpc_client_destroy(client);
            client = NULL;
        }
    }

    it("should get current block number") {
        rpc_call_result_t result;
        int ret = rpc_client_call(client, "eth_blockNumber", "[]", &result);

        check(ret == 0);
        check(result.success);
        check(result.result != NULL);
        
        // Basic validation of hex string result
        if (result.result) {
            check(strlen(result.result) > 0);
            // It might be quoted string like "0x..."
            char *val = result.result;
            if (val[0] == '"') val++;
            check(strncmp(val, "0x", 2) == 0);
        }

        rpc_result_free(&result);
    }

    it("should get chain ID") {
        rpc_call_result_t result;
        int ret = rpc_client_call(client, "eth_chainId", "[]", &result);

        check(ret == 0);
        check(result.success);
        check(result.result != NULL);

        // 0x1 is simple mainnet id, but let's just check it's a hex value
        if (result.result) {
            char *val = result.result;
            if (val[0] == '"') val++;
            check(strncmp(val, "0x", 2) == 0);
        }

        rpc_result_free(&result);
    }

    it("should get gas price") {
        rpc_call_result_t result;
        int ret = rpc_client_call(client, "eth_gasPrice", "[]", &result);

        check(ret == 0);
        check(result.success);
        check(result.result != NULL);

        if (result.result) {
            char *val = result.result;
            if (val[0] == '"') val++;
            check(strncmp(val, "0x", 2) == 0);
        }

        rpc_result_free(&result);
    }

    it("should handle invalid method errors") {
        rpc_call_result_t result;
        int ret = rpc_client_call(client, "invalid_method_xyz", "[]", &result);

        check(ret == 0);         // Transport successful
        check(!result.success);  // RPC call failed
        check(result.error_code != 0);
        check(result.error_message != NULL);

        rpc_result_free(&result);
    }

    it("should track statistics") {
        rpc_stats_t *stats = rpc_stats_create();
        check(stats != NULL);

        uint64_t req_id = rpc_stats_request_start(stats);
        
        rpc_call_result_t result;
        int ret = rpc_client_call(client, "net_version", "[]", &result);
        
        if (ret == 0 && result.success) {
            size_t bytes_sent = 50; // Approxiate
            size_t bytes_recv = result.result ? strlen(result.result) : 0;
            rpc_stats_request_success(stats, req_id, bytes_sent, bytes_recv);
        } else {
            rpc_stats_request_failure(stats, req_id, RPC_ERROR_NETWORK);
        }
        
        rpc_result_free(&result);

        // Can't easily inspect stats internals without accessors, 
        // but we verify the API flow works.
        rpc_stats_destroy(stats);
    }

    it("should configure retry policy") {
        rpc_retry_policy_t retry_policy = rpc_retry_policy_default();
        retry_policy.max_retries = 3;
        retry_policy.initial_delay_ms = 500;
        
        check(retry_policy.max_retries == 3);
        check(retry_policy.initial_delay_ms == 500);

        rpc_retry_state_t state;
        rpc_retry_state_init(&state, &retry_policy);
        
        check(!rpc_retry_budget_exhausted(&state, &retry_policy));
        
        rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);
        // After 1 attempt, budget should not be exhausted (max_retries=3)
        check(!rpc_retry_budget_exhausted(&state, &retry_policy));
    }

    it("should get account balance") {
         const char *eth_foundation = "0xde0B295669a9FD93d5F28D9Ec85E40f4cb697BAe";
         char params[256];
         snprintf(params, sizeof(params), "[\"%s\", \"latest\"]", eth_foundation);

         rpc_call_result_t result;
         int ret = rpc_client_call(client, "eth_getBalance", params, &result);

         check(ret == 0);
         check(result.success);
         check(result.result != NULL);

         if (result.result) {
            char *val = result.result;
            if (val[0] == '"') val++;
            check(strncmp(val, "0x", 2) == 0);
         }

         rpc_result_free(&result);
    }
}
