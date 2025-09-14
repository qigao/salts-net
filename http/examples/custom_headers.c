#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const char* url = "http://httpbin.org/headers";
    
    printf("Testing custom headers\n\n");
    
    // Create client
    http_client_t* client = http_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }
    
    // Set custom user agent
    http_client_set_user_agent(client, "MyApp/2.0");
    
    // Custom headers
    const char* headers[] = {
        "X-Custom-Header: MyValue",
        "X-Request-ID: 12345",
        "Accept: application/json"
    };
    
    // Make request
    http_response_t* response = http_request(client, HTTP_GET, url,
                                             headers, 3,
                                             NULL, 0);
    
    // Check for errors
    if (response->error) {
        fprintf(stderr, "Error: %s\n", response->error);
        http_response_free(response);
        http_client_destroy(client);
        return 1;
    }
    
    // Print response
    printf("Status: %d\n", response->status_code);
    printf("\nBody:\n%s\n", response->body);
    
    // Cleanup
    http_response_free(response);
    http_client_destroy(client);
    
    return 0;
}
