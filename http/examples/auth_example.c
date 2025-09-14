#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    http_client_t* client = http_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create HTTP client\n");
        return 1;
    }

    printf("=== Basic Authentication Example ===\n\n");
    
    // Example 1: Basic Auth
    printf("1. Testing Basic Auth with httpbin.org...\n");
    http_client_set_basic_auth(client, "user", "passwd");
    
    http_response_t* response = http_get(client, "https://httpbin.org/basic-auth/user/passwd");
    if (response->error) {
        printf("Error: %s (code: %d)\n", response->error, response->error_code);
    } else {
        printf("Status: %d\n", response->status_code);
        printf("Body: %s\n", response->body);
    }
    http_response_free(response);
    
    printf("\n");
    
    // Example 2: Bearer Token
    printf("2. Testing Bearer Token with httpbin.org...\n");
    http_client_clear_auth(client);
    http_client_set_bearer_token(client, "my-secret-token-12345");
    
    response = http_get(client, "https://httpbin.org/bearer");
    if (response->error) {
        printf("Error: %s (code: %d)\n", response->error, response->error_code);
    } else {
        printf("Status: %d\n", response->status_code);
        printf("Body: %s\n", response->body);
    }
    http_response_free(response);
    
    printf("\n");
    
    // Example 3: No Auth
    printf("3. Testing without authentication...\n");
    http_client_clear_auth(client);
    
    response = http_get(client, "https://httpbin.org/get");
    if (response->error) {
        printf("Error: %s (code: %d)\n", response->error, response->error_code);
    } else {
        printf("Status: %d\n", response->status_code);
        printf("Body length: %zu bytes\n", response->body_len);
    }
    http_response_free(response);

    http_client_destroy(client);
    return 0;
}
