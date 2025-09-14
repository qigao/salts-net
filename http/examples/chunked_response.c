#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    // This endpoint returns chunked transfer encoding
    const char* url = "http://httpbin.org/stream/5";
    
    printf("Testing chunked response handling\n");
    printf("URL: %s\n\n", url);
    
    // Create client
    http_client_t* client = http_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }
    
    // Make request
    http_response_t* response = http_get(client, url);
    
    // Check for errors
    if (response->error) {
        fprintf(stderr, "Error: %s\n", response->error);
        http_response_free(response);
        http_client_destroy(client);
        return 1;
    }
    
    // Print response
    printf("Status: %d\n", response->status_code);
    printf("Body length: %zu bytes\n", response->body_len);
    printf("\nBody:\n%s\n", response->body);
    
    printf("\nNote: llhttp automatically handles chunked encoding!\n");
    
    // Cleanup
    http_response_free(response);
    http_client_destroy(client);
    
    return 0;
}
