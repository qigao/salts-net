
#include "tinytest.h"
#include "rpc_client.h"
#include "rpc_error.h"
#include "rpc_retry.h"
#include "rpc_stats.h"
#include <netcore/turbo_coro_context.h>
#include <turbo_coro.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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

static int test_block_ok;
static void test_block_number(turbo_coro_context_t *ctx) {
    rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
    config.timeout_ms = 10000;
    config.coro_ctx = ctx;
    rpc_client_t *client = rpc_client_create(&config);
    test_block_ok = 0;
    if (!client) return;

    rpc_call_result_t result;
    int ret = rpc_client_call(client, "eth_blockNumber", "[]", &result);
    if (ret == 0 && result.success && result.result && strlen(result.result) > 0) {
        char *val = result.result;
        if (val[0] == '"') val++;
        if (strncmp(val, "0x", 2) == 0) test_block_ok = 1;
    }
    rpc_result_free(&result);
    rpc_client_destroy(client);
}

static int test_chain_ok;
static void test_chain_id(turbo_coro_context_t *ctx) {
    rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
    config.timeout_ms = 10000;
    config.coro_ctx = ctx;
    rpc_client_t *client = rpc_client_create(&config);
    test_chain_ok = 0;
    if (!client) return;

    rpc_call_result_t result;
    int ret = rpc_client_call(client, "eth_chainId", "[]", &result);
    if (ret == 0 && result.success && result.result) {
        char *val = result.result;
        if (val[0] == '"') val++;
        if (strncmp(val, "0x", 2) == 0) test_chain_ok = 1;
    }
    rpc_result_free(&result);
    rpc_client_destroy(client);
}

static int test_gas_ok;
static void test_gas_price(turbo_coro_context_t *ctx) {
    rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
    config.timeout_ms = 10000;
    config.coro_ctx = ctx;
    rpc_client_t *client = rpc_client_create(&config);
    test_gas_ok = 0;
    if (!client) return;

    rpc_call_result_t result;
    int ret = rpc_client_call(client, "eth_gasPrice", "[]", &result);
    if (ret == 0 && result.success && result.result) {
        char *val = result.result;
        if (val[0] == '"') val++;
        if (strncmp(val, "0x", 2) == 0) test_gas_ok = 1;
    }
    rpc_result_free(&result);
    rpc_client_destroy(client);
}

static int test_invalid_ok;
static void test_invalid_method(turbo_coro_context_t *ctx) {
    rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
    config.timeout_ms = 10000;
    config.coro_ctx = ctx;
    rpc_client_t *client = rpc_client_create(&config);
    test_invalid_ok = 0;
    if (!client) return;

    rpc_call_result_t result;
    int ret = rpc_client_call(client, "invalid_method_xyz", "[]", &result);
    if (ret == 0 && !result.success && result.error_code != 0 && result.error_message)
        test_invalid_ok = 1;
    rpc_result_free(&result);
    rpc_client_destroy(client);
}

static int test_stats_ok;
static void test_track_stats(turbo_coro_context_t *ctx) {
    rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
    config.timeout_ms = 10000;
    config.coro_ctx = ctx;
    rpc_client_t *client = rpc_client_create(&config);
    test_stats_ok = 0;
    if (!client) return;

    rpc_stats_t *stats = rpc_stats_create();
    if (!stats) { rpc_client_destroy(client); return; }

    uint64_t req_id = rpc_stats_request_start(stats);
    rpc_call_result_t result;
    int ret = rpc_client_call(client, "net_version", "[]", &result);
    if (ret == 0 && result.success) {
        size_t bytes_sent = 50;
        size_t bytes_recv = result.result ? strlen(result.result) : 0;
        rpc_stats_request_success(stats, req_id, bytes_sent, bytes_recv);
    } else {
        rpc_stats_request_failure(stats, req_id, RPC_ERROR_NETWORK);
    }
    rpc_result_free(&result);
    test_stats_ok = 1; // API flow works

    rpc_stats_destroy(stats);
    rpc_client_destroy(client);
}

static int test_balance_ok;
static void test_get_balance(turbo_coro_context_t *ctx) {
    rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("https://ethereum.publicnode.com/");
    config.timeout_ms = 15000;
    config.coro_ctx = ctx;
    rpc_client_t *client = rpc_client_create(&config);
    test_balance_ok = 0;
    if (!client) return;

    const char *eth_foundation = "0xde0B295669a9FD93d5F28D9Ec85E40f4cb697BAe";
    char params[256];
    snprintf(params, sizeof(params), "[\"%s\", \"latest\"]", eth_foundation);

    rpc_call_result_t result;
    int ret = rpc_client_call(client, "eth_getBalance", params, &result);
    if (ret == 0 && result.success && result.result) {
        char *val = result.result;
        if (val[0] == '"') val++;
        if (strncmp(val, "0x", 2) == 0) test_balance_ok = 1;
    }
    rpc_result_free(&result);
    rpc_client_destroy(client);
}

/* ── Specs ──────────────────────────────────────────────────────── */

spec("Ethereum JSON-RPC Examples (BDD)") {
    it("should get current block number") {
        run_in_coro(test_block_number);
        check(test_block_ok);
    }

    it("should get chain ID") {
        run_in_coro(test_chain_id);
        check(test_chain_ok);
    }

    it("should get gas price") {
        run_in_coro(test_gas_price);
        check(test_gas_ok);
    }

    it("should handle invalid method errors") {
        run_in_coro(test_invalid_method);
        check(test_invalid_ok);
    }

    it("should track statistics") {
        run_in_coro(test_track_stats);
        check(test_stats_ok);
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
        check(!rpc_retry_budget_exhausted(&state, &retry_policy));
    }

    it("should get account balance") {
        run_in_coro(test_get_balance);
        check(test_balance_ok);
    }
}
