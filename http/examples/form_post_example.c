#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    http_client_t* client = http_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create HTTP client\n");
        return 1;
    }

    printf("=== URL-Encoded Form POST Example ===\n\n");
    
    // Create form parameters
    http_params_t* params = http_params_create();
    http_params_add(params, "name", "John Doe");
    http_params_add(params, "email", "john@example.com");
    http_params_add(params, "message", "Hello from HTTP client!");
    
    // POST form data
    printf("Posting form data to httpbin.org...\n");
    http_response_t* response = http_post_form(client, 
                                               "https://httpbin.org/post",
                                               params);
    
    if (response->error) {
        printf("Error: %s (code: %d)\n", response->error, response->error_code);
    } else {
        printf("Status: %d\n", response->status_code);
        printf("\nResponse:\n%s\n", response->body);
    }
    
    http_response_free(response);
    http_params_free(params);
    
    printf("\n=== URL Building with Query Parameters ===\n\n");
    
    // Build URL with query parameters
    http_params_t* query = http_params_create();
    http_params_add(query, "search", "http client");
    http_params_add(query, "page", "1");
    http_params_add(query, "limit", "10");
    
    char* url = http_build_url("https://httpbin.org/get", query);
    printf("Built URL: %s\n\n", url);
    
    response = http_get(client, url);
    
    if (!response->error) {
        printf("Status: %d\n", response->status_code);
        printf("Response length: %zu bytes\n", response->body_len);
    }
    
    http_response_free(response);
    http_params_free(query);
    free(url);
    
    http_client_destroy(client);
    return 0;
}
