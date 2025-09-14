/**
 * @file eth_rpc_example.c
 * @brief Example using Public Node Ethereum JSON-RPC endpoint
 *
 * Demonstrates:
 * - Making real JSON-RPC 2.0 calls to Ethereum network
 * - Error handling for different scenarios
 * - Retry logic with exponential backoff
 * - Statistics tracking for monitoring
 *
 * Uses: https://ethereum.publicnode.com (free, no authentication required)
 */

#include "rpc_client.h"
#include "rpc_error.h"
#include "rpc_retry.h"
#include "rpc_stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#include <stb_sprintf.h>
#define sleep_ms(ms) usleep((ms)*1000)
#endif

// ============================================================================
// Example 1: Basic JSON-RPC call - Get current block number
// ============================================================================

void example_get_block_number(void) {
  printf("\n=== Example 1: Get Ethereum Block Number ===\n");
  printf("Using Public Node Ethereum JSON-RPC endpoint\n\n");

  // Create RPC client for Public Node
  // Note: Using HTTP for testing (most production endpoints require HTTPS)
  rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("ethereum.publicnode.com", 443);
  config.endpoint = "/";
  config.transport = RPC_TRANSPORT_TLS;
  config.timeout_ms = 10000; // 10 seconds

  printf("[DEBUG] Creating RPC client...\n");
  rpc_client_t *client = rpc_client_create(&config);
  if (!client) {
    printf("Failed to create RPC client\n");
    return;
  }
  printf("[DEBUG] RPC client created successfully\n");

  // Make JSON-RPC call - eth_blockNumber returns latest block number
  printf("Calling method: eth_blockNumber\n");
  printf("Parameters: [] (empty array)\n\n");

  printf("[DEBUG] Making RPC call...\n");
  rpc_call_result_t result;
  int ret = rpc_client_call(client, "eth_blockNumber", "[]", &result);
  printf("[DEBUG] RPC call returned: %d\n", ret);

  if (ret == 0) {
    printf("HTTP Status: %d\n", result.http_status);

    if (result.success && result.result) {
      printf("Block Number (hex): %s\n", result.result);
      // Convert hex to decimal for display
      if (result.result[0] == '"') {
        // Remove quotes if present
        char *hex_str = result.result + 1;
        char *end = strchr(hex_str, '"');
        if (end) *end = '\0';
        if (strncmp(hex_str, "0x", 2) == 0) {
          unsigned long long block_num = strtoull(hex_str + 2, NULL, 16);
          printf("Block Number (decimal): %llu\n", block_num);
        }
      }
    } else if (result.error_message) {
      printf("Error: %s (code: %d)\n", result.error_message, result.error_code);
    }
  } else {
    printf("Request failed with code: %d\n", ret);
  }

  rpc_result_free(&result);
  rpc_client_destroy(client);
}

// ============================================================================
// Example 2: Get chain ID
// ============================================================================

void example_get_chain_id(void) {
  printf("\n=== Example 2: Get Ethereum Chain ID ===\n");

  rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("ethereum.publicnode.com", 443);
  config.endpoint = "/";
  config.transport = RPC_TRANSPORT_TLS;

  rpc_client_t *client = rpc_client_create(&config);
  if (!client) {
    printf("Failed to create client\n");
    return;
  }

  rpc_call_result_t result;
  int ret = rpc_client_call(client, "eth_chainId", "[]", &result);

  if (ret == 0 && result.success) {
    printf("Chain ID: %s\n", result.result);
    // 0x1 = Ethereum Mainnet
  } else {
    printf("Failed to get chain ID\n");
  }

  rpc_result_free(&result);
  rpc_client_destroy(client);
}

// ============================================================================
// Example 3: Get gas price
// ============================================================================

void example_get_gas_price(void) {
  printf("\n=== Example 3: Get Current Gas Price ===\n");

  rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("ethereum.publicnode.com", 443);
  config.endpoint = "/";
  config.transport = RPC_TRANSPORT_TLS;

  rpc_client_t *client = rpc_client_create(&config);
  if (!client) {
    printf("Failed to create client\n");
    return;
  }

  rpc_call_result_t result;
  int ret = rpc_client_call(client, "eth_gasPrice", "[]", &result);

  if (ret == 0 && result.success) {
    printf("Gas Price (hex wei): %s\n", result.result);
    // Convert to Gwei for readability
    if (result.result[0] == '"') {
      char *hex_str = result.result + 1;
      char *end = strchr(hex_str, '"');
      if (end) *end = '\0';
      if (strncmp(hex_str, "0x", 2) == 0) {
        unsigned long long gas_wei = strtoull(hex_str + 2, NULL, 16);
        double gas_gwei = (double)gas_wei / 1e9;
        printf("Gas Price: %.2f Gwei\n", gas_gwei);
      }
    }
  } else {
    printf("Failed to get gas price\n");
  }

  rpc_result_free(&result);
  rpc_client_destroy(client);
}

// ============================================================================
// Example 4: Error handling - Invalid method
// ============================================================================

void example_error_handling(void) {
  printf("\n=== Example 4: Error Handling ===\n");
  printf("Testing JSON-RPC error responses\n\n");

  rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("ethereum.publicnode.com", 443);
  config.endpoint = "/";
  config.transport = RPC_TRANSPORT_TLS;

  rpc_client_t *client = rpc_client_create(&config);
  if (!client) {
    printf("Failed to create client\n");
    return;
  }

  // Call non-existent method
  printf("Calling invalid method: invalid_method_xyz\n");
  rpc_call_result_t result;
  int ret = rpc_client_call(client, "invalid_method_xyz", "[]", &result);

  if (ret == 0) {
    printf("HTTP Status: %d\n", result.http_status);
    if (!result.success) {
      printf("JSON-RPC Error Code: %d\n", result.error_code);
      printf("Error Message: %s\n", result.error_message ? result.error_message : "N/A");

      rpc_error_code_t err_code = rpc_error_from_http_status(result.http_status);
      printf("RPC Error Category: %s\n", rpc_error_name(err_code));
      printf("Is Retryable: %s\n", rpc_error_is_retryable(err_code) ? "Yes" : "No");
    }
  }

  rpc_result_free(&result);
  rpc_client_destroy(client);
}

// ============================================================================
// Example 5: Statistics tracking with multiple requests
// ============================================================================

void example_statistics(void) {
  printf("\n=== Example 5: Statistics Tracking ===\n");
  printf("Making multiple requests and tracking performance\n\n");

  rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("ethereum.publicnode.com", 443);
  config.endpoint = "/";
  config.transport = RPC_TRANSPORT_TLS;

  rpc_client_t *client = rpc_client_create(&config);
  if (!client) {
    printf("Failed to create client\n");
    return;
  }

  rpc_stats_t *stats = rpc_stats_create();
  if (!stats) {
    rpc_client_destroy(client);
    return;
  }

  const char *methods[] = {
    "eth_blockNumber",
    "eth_chainId",
    "eth_gasPrice",
    "net_version",
    "web3_clientVersion"
  };
  int num_methods = sizeof(methods) / sizeof(methods[0]);

  printf("Making %d requests...\n", num_methods);

  for (int i = 0; i < num_methods; i++) {
    uint64_t req_id = rpc_stats_request_start(stats);

    rpc_call_result_t result;
    int ret = rpc_client_call(client, methods[i], "[]", &result);

    if (ret == 0 && result.http_status == 200 && result.success) {
      size_t bytes_sent = strlen(methods[i]) + 50; // Approximate
      size_t bytes_recv = result.result ? strlen(result.result) : 0;
      rpc_stats_request_success(stats, req_id, bytes_sent, bytes_recv);
      printf("  %s: OK\n", methods[i]);
    } else {
      rpc_stats_request_failure(stats, req_id, RPC_ERROR_NETWORK);
      printf("  %s: FAILED\n", methods[i]);
    }

    rpc_result_free(&result);
  }

  // Print statistics
  printf("\n");
  rpc_stats_report(stats, stdout);

  rpc_stats_destroy(stats);
  rpc_client_destroy(client);
}

// ============================================================================
// Example 6: Retry logic demonstration
// ============================================================================

void example_retry_logic(void) {
  printf("\n=== Example 6: Retry Logic Configuration ===\n");
  printf("Demonstrating retry policy setup (without actual retries)\n\n");

  rpc_retry_policy_t retry_policy = rpc_retry_policy_default();
  retry_policy.max_retries = 3;
  retry_policy.initial_delay_ms = 500;
  retry_policy.max_delay_ms = 5000;
  retry_policy.backoff_multiplier = 2.0;
  retry_policy.jitter_factor = 0.1;
  retry_policy.retry_on_http_5xx = 1;

  printf("Retry Policy Configuration:\n");
  printf("  Max Retries: %d\n", retry_policy.max_retries);
  printf("  Initial Delay: %u ms\n", retry_policy.initial_delay_ms);
  printf("  Max Delay: %u ms\n", retry_policy.max_delay_ms);
  printf("  Backoff Multiplier: %.1f\n", retry_policy.backoff_multiplier);
  printf("  Jitter Factor: %.2f\n", retry_policy.jitter_factor);
  printf("  Retry on 5xx: %s\n", retry_policy.retry_on_http_5xx ? "Yes" : "No");

  rpc_retry_state_t state;
  rpc_retry_state_init(&state, &retry_policy);

  printf("\nSimulated retry delays:\n");
  for (int i = 0; i < 3; i++) {
    uint32_t delay = rpc_retry_get_delay(&state, &retry_policy);
    printf("  Attempt %d: wait %u ms\n", i + 1, delay);
    rpc_retry_record_attempt(&state, RPC_ERROR_TIMEOUT);
  }

  printf("\nBudget exhausted: %s\n",
         rpc_retry_budget_exhausted(&state, &retry_policy) ? "Yes" : "No");
}

// ============================================================================
// Example 7: Get account balance
// ============================================================================

void example_get_balance(void) {
  printf("\n=== Example 7: Get ETH Balance ===\n");

  rpc_client_config_t config = RPC_CLIENT_DEFAULT_CONFIG("ethereum.publicnode.com", 443);
  config.endpoint = "/";
  config.transport = RPC_TRANSPORT_TLS;

  rpc_client_t *client = rpc_client_create(&config);
  if (!client) {
    printf("Failed to create client\n");
    return;
  }

  // Get balance of Ethereum Foundation address
  const char *eth_foundation = "0xde0B295669a9FD93d5F28D9Ec85E40f4cb697BAe";
  char params[256];
  stbsp_snprintf(params, sizeof(params), "[\"%s\", \"latest\"]", eth_foundation);

  printf("Getting balance for Ethereum Foundation\n");
  printf("Address: %s\n", eth_foundation);

  rpc_call_result_t result;
  int ret = rpc_client_call(client, "eth_getBalance", params, &result);

  if (ret == 0 && result.success) {
    printf("Balance (hex wei): %s\n", result.result);
    // Convert to ETH
    if (result.result[0] == '"') {
      char *hex_str = result.result + 1;
      char *end = strchr(hex_str, '"');
      if (end) *end = '\0';
      if (strncmp(hex_str, "0x", 2) == 0) {
        // Note: This is a simplified conversion, actual ETH amounts may need BigInt
        unsigned long long balance_wei = strtoull(hex_str + 2, NULL, 16);
        double balance_eth = (double)balance_wei / 1e18;
        printf("Balance: %.6f ETH\n", balance_eth);
      }
    }
  } else {
    printf("Failed to get balance\n");
  }

  rpc_result_free(&result);
  rpc_client_destroy(client);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
  printf("========================================\n");
  printf("  RPC Client - Ethereum JSON-RPC Examples\n");
  printf("========================================\n");
  printf("\nUsing HTTPS Ethereum JSON-RPC endpoint\n");
  printf("https://ethereum.publicnode.com\n");
  printf("\nThis endpoint provides free access to Ethereum mainnet.\n");

  // Run examples
  example_get_block_number();
  example_get_chain_id();
  example_get_gas_price();
  example_error_handling();
  example_statistics();
  example_retry_logic();
  example_get_balance();

  printf("\n========================================\n");
  printf("  All examples completed!\n");
  printf("========================================\n");

  return 0;
}
