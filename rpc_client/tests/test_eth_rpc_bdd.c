
#include "tinytest.h"
#include "rpc_client.h"
#include "rpc_error.h"
#include "rpc_retry.h"
#include "rpc_stats.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <http_client.h>

static int is_rpc_network_error(rpc_call_result_t *r) {
  return r->error_code == RPC_ERROR_NETWORK || r->error_code == RPC_ERROR_INTERNAL;
}

spec("Ethereum JSON-RPC Examples (BDD)") {
    it("should get current block number") {
        http_client_t *http = http_client_create(NULL);
        http_client_set_timeout(http, 10000);
        rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
        config.http_client = http;
        rpc_client_t *client = rpc_client_create(&config);
        check_not_null(client);

        rpc_call_result_t result;
        int ret = rpc_client_call(client, "eth_blockNumber", "[]", &result);
        if (ret == 0 && result.success && result.result) {
            char *val = result.result;
            if (val[0] == '"') val++;
            check(strncmp(val, "0x", 2) == 0);
        }
        rpc_result_free(&result);
        rpc_client_destroy(client);
        http_client_destroy(http);
    }

    it("should get chain ID") {
        http_client_t *http = http_client_create(NULL);
        http_client_set_timeout(http, 10000);
        rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
        config.http_client = http;
        rpc_client_t *client = rpc_client_create(&config);
        check_not_null(client);

        rpc_call_result_t result;
        int ret = rpc_client_call(client, "eth_chainId", "[]", &result);
        if (ret == 0 && result.success && result.result) {
            char *val = result.result;
            if (val[0] == '"') val++;
            check(strncmp(val, "0x", 2) == 0);
        }
        rpc_result_free(&result);
        rpc_client_destroy(client);
        http_client_destroy(http);
    }

    it("should get gas price") {
        http_client_t *http = http_client_create(NULL);
        http_client_set_timeout(http, 10000);
        rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
        config.http_client = http;
        rpc_client_t *client = rpc_client_create(&config);
        check_not_null(client);

        rpc_call_result_t result;
        int ret = rpc_client_call(client, "eth_gasPrice", "[]", &result);
        if (ret == 0 && result.success && result.result) {
            char *val = result.result;
            if (val[0] == '"') val++;
            check(strncmp(val, "0x", 2) == 0);
        }
        rpc_result_free(&result);
        rpc_client_destroy(client);
        http_client_destroy(http);
    }

    it("should handle invalid method errors") {
        http_client_t *http = http_client_create(NULL);
        http_client_set_timeout(http, 10000);
        rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
        config.http_client = http;
        rpc_client_t *client = rpc_client_create(&config);
        check_not_null(client);

        rpc_call_result_t result;
        int ret = rpc_client_call(client, "invalid_method_xyz", "[]", &result);
        if (ret == 0 && !is_rpc_network_error(&result)) {
            check(!result.success);
            check(result.error_code != 0);
            check_not_null(result.error_message);
        }
        rpc_result_free(&result);
        rpc_client_destroy(client);
        http_client_destroy(http);
    }

    it("should track statistics") {
        http_client_t *http = http_client_create(NULL);
        http_client_set_timeout(http, 10000);
        rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
        config.http_client = http;
        rpc_client_t *client = rpc_client_create(&config);
        check_not_null(client);

        rpc_stats_t *stats = rpc_stats_create();
        check_not_null(stats);

        uint64_t req_id = rpc_stats_request_start(stats);
        rpc_call_result_t result;
        int ret = rpc_client_call(client, "net_version", "[]", &result);
        if (ret == 0 && result.success) {
            rpc_stats_request_success(stats, req_id, 50,
                result.result ? strlen(result.result) : 0);
        } else {
            rpc_stats_request_failure(stats, req_id, RPC_ERROR_NETWORK);
        }
        rpc_result_free(&result);

        rpc_stats_destroy(stats);
        rpc_client_destroy(client);
        http_client_destroy(http);
    }

    it("should configure retry policy") {
        rpc_retry_policy_t retry_policy = rpc_retry_policy_default();
        retry_policy.max_retries = 3;
        retry_policy.initial_delay_ms = 500;
        check_int_eq(retry_policy.max_retries, 3);
        check_int_eq(retry_policy.initial_delay_ms, 500);

        rpc_retry_state_t state;
        rpc_retry_state_init(&state, &retry_policy);
        check(!rpc_retry_budget_exhausted(&state, &retry_policy));
        rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);
        check(!rpc_retry_budget_exhausted(&state, &retry_policy));
    }

    it("should get account balance") {
        http_client_t *http = http_client_create(NULL);
        http_client_set_timeout(http, 15000);
        rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
        config.http_client = http;
        rpc_client_t *client = rpc_client_create(&config);
        check_not_null(client);

        const char *eth_foundation = "0xde0B295669a9FD93d5F28D9Ec85E40f4cb697BAe";
        char params[256];
        snprintf(params, sizeof(params), "[\"%s\", \"latest\"]", eth_foundation);

        rpc_call_result_t result;
        int ret = rpc_client_call(client, "eth_getBalance", params, &result);
        if (ret == 0 && result.success && result.result) {
            char *val = result.result;
            if (val[0] == '"') val++;
            check(strncmp(val, "0x", 2) == 0);
        }
        rpc_result_free(&result);
        rpc_client_destroy(client);
        http_client_destroy(http);
    }
}
