#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void) {
    printf("=== Retry Policy Example ===\n\n");
    
    http_client_t* client = http_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create HTTP client\n");
        return 1;
    }

    printf("1. Testing without retry policy (will fail on 500 error)...\n");
    
    // Make request to endpoint that returns 500
    http_response_t* response = http_get(client, "https://httpbin.org/status/500");
    
    if (response->error) {
        printf("   Error: %s\n", response->error);
    } else {
        printf("   Status: %d (no retry, failed immediately)\n", response->status_code);
    }
    
    http_response_free(response);
    
    printf("\n2. Setting up retry policy...\n");
    
    // Create and set retry policy
    http_retry_policy_t policy = http_retry_policy_default();
    policy.max_retries = 3;
    policy.initial_delay_ms = 500;  // Start with 500ms
    policy.exponential_backoff = 1;
    policy.retry_on_5xx = 1;
    
    http_client_set_retry_policy(client, &policy);
    
    printf("   Max retries: %d\n", policy.max_retries);
    printf("   Initial delay: %dms\n", policy.initial_delay_ms);
    printf("   Exponential backoff: %s\n", policy.exponential_backoff ? "Yes" : "No");
    printf("   Retry on 5xx: %s\n", policy.retry_on_5xx ? "Yes" : "No");
    
    printf("\n3. Testing retry on 500 error (will retry 3 times)...\n");
    
    clock_t start = clock();
    response = http_get(client, "https://httpbin.org/status/500");
    clock_t end = clock();
    
    double elapsed = ((double)(end - start)) / CLOCKS_PER_SEC;
    
    if (response->error) {
        printf("   Error: %s\n", response->error);
    } else {
        printf("   Final status: %d\n", response->status_code);
    }
    printf("   Total time: %.2f seconds (includes retries)\n", elapsed);
    
    http_response_free(response);
    
    printf("\n4. Testing successful request (no retry needed)...\n");
    
    start = clock();
    response = http_get(client, "https://httpbin.org/get");
    end = clock();
    
    elapsed = ((double)(end - start)) / CLOCKS_PER_SEC;
    
    if (!response->error) {
        printf("   Status: %d (succeeded on first try)\n", response->status_code);
        printf("   Time: %.2f seconds\n", elapsed);
    }
    
    http_response_free(response);
    
    printf("\n5. Testing retry on connection error...\n");
    
    // Configure to retry on connection errors
    policy.retry_on_connection_error = 1;
    policy.max_retries = 2;
    policy.initial_delay_ms = 200;
    http_client_set_retry_policy(client, &policy);
    
    // Try to connect to non-existent host
    start = clock();
    response = http_get(client, "https://this-host-does-not-exist-12345.com");
    end = clock();
    
    elapsed = ((double)(end - start)) / CLOCKS_PER_SEC;
    
    printf("   Error: %s\n", response->error);
    printf("   Time: %.2f seconds (tried %d times)\n", elapsed, policy.max_retries + 1);
    
    http_response_free(response);
    
    printf("\n6. Testing custom retry policy...\n");
    
    // Custom policy: more aggressive retries
    http_retry_policy_t custom_policy = {
        .max_retries = 5,
        .initial_delay_ms = 100,
        .max_delay_ms = 5000,
        .exponential_backoff = 1,
        .retry_on_timeout = 1,
        .retry_on_connection_error = 1,
        .retry_on_5xx = 1,
        .jitter_factor = 0.2  // 20% jitter
    };
    
    http_client_set_retry_policy(client, &custom_policy);
    
    printf("   Custom policy set:\n");
    printf("   - Max retries: %d\n", custom_policy.max_retries);
    printf("   - Initial delay: %dms\n", custom_policy.initial_delay_ms);
    printf("   - Max delay: %dms\n", custom_policy.max_delay_ms);
    printf("   - Jitter factor: %.1f\n", custom_policy.jitter_factor);
    
    printf("\n7. Clearing retry policy...\n");
    
    http_client_clear_retry_policy(client);
    
    response = http_get(client, "https://httpbin.org/status/503");
    
    if (!response->error) {
        printf("   Status: %d (no retry after clearing policy)\n", response->status_code);
    }
    
    http_response_free(response);
    
    // Cleanup
    http_client_destroy(client);
    
    printf("\n Retry policy example complete!\n");
    printf("\nKey takeaways:\n");
    printf("- Retry policies automatically retry failed requests\n");
    printf("- Exponential backoff prevents overwhelming servers\n");
    printf("- Jitter prevents thundering herd problem\n");
    printf("- Can retry on timeouts, connection errors, and 5xx errors\n");
    
    return 0;
}
