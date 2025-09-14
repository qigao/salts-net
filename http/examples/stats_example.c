#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    http_client_t* client = http_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create HTTP client\n");
        return 1;
    }

    printf("=== HTTP Client Statistics Example ===\n\n");
    
    // Make several requests
    printf("Making multiple requests...\n");
    
    for (int i = 0; i < 3; i++) {
        printf("  Request %d...\n", i + 1);
        http_response_t* response = http_get(client, "https://httpbin.org/get");
        
        if (!response->error) {
            printf("    Status: %d, Body size: %zu bytes\n", 
                   response->status_code, response->body_len);
        } else {
            printf("    Error: %s\n", response->error);
        }
        
        http_response_free(response);
    }
    
    // Get statistics
    printf("\n=== Client Statistics ===\n");
    http_client_stats_t stats;
    http_client_get_stats(client, &stats);
    
    printf("Total requests:       %llu\n", (unsigned long long)stats.total_requests);
    printf("Successful requests:  %llu\n", (unsigned long long)stats.successful_requests);
    printf("Failed requests:      %llu\n", (unsigned long long)stats.failed_requests);
    printf("Redirects followed:   %llu\n", (unsigned long long)stats.redirects_followed);
    printf("Bytes sent:           %llu\n", (unsigned long long)stats.bytes_sent);
    printf("Bytes received:       %llu\n", (unsigned long long)stats.bytes_received);
    printf("Connections created:  %llu\n", (unsigned long long)stats.connections_created);
    printf("Connections reused:   %llu\n", (unsigned long long)stats.connections_reused);
    
    // Test redirect tracking
    printf("\n=== Testing Redirect Tracking ===\n");
    http_response_t* response = http_get(client, "https://httpbin.org/redirect/2");
    
    if (!response->error) {
        printf("Final status: %d\n", response->status_code);
    }
    
    http_response_free(response);
    
    // Get updated stats
    http_client_get_stats(client, &stats);
    printf("Redirects followed:   %llu\n", (unsigned long long)stats.redirects_followed);
    
    // Reset stats
    printf("\n=== Resetting Statistics ===\n");
    http_client_reset_stats(client);
    http_client_get_stats(client, &stats);
    printf("Total requests after reset: %llu\n", (unsigned long long)stats.total_requests);
    
    http_client_destroy(client);
    return 0;
}
