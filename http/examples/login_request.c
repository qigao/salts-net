#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Example demonstrating a login request using the TurboNet HTTP client.
 * 
 * Target URL: http://82.156.254.2/v1/auth/login
 * Request Body: JSON with api_key and client_id
 */

int main(void) {
    // 1. Define login parameters
    const char* base_url = "http://82.156.254.2";
    const char* login_endpoint = "/v1/auth/login";
    
    // Construct full URL
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", base_url, login_endpoint);

    // Request body provided by user
    const char* login_json = 
        "{"
        "\"api_key\":\"$2a$10$Y7N5Mei5yfIjUS2QJ56zfOZRjKC0pOuwWybp3wesGMQwh2MLvI3Z2\","
        "\"client_id\":\"b1a2c3d4-2222-4000-8000-000000000002\""
        "}";

    printf("--- TurboNet Authentication Example ---\n");
    printf("Logging in to: %s\n", full_url);
    printf("Payload size:  %zu bytes\n\n", strlen(login_json));

    // 2. Initialize the HTTP client
    http_client_t* client = http_client_create();
    if (!client) {
        fprintf(stderr, "Failed to initialize TurboNet HTTP client.\n");
        return 1;
    }

    // Set a reasonable timeout
    http_client_set_timeout(client, 5000); // 5 seconds
    
    // 3. Prepare headers
    // For JSON requests, we must set Content-Type
    const char* headers[] = {
        "Content-Type: application/json",
        "Accept: application/json",
        "User-Agent: TurboNet-Client/1.0"
    };
    int header_count = sizeof(headers) / sizeof(headers[0]);

    // 4. Execute the POST request
    printf("[*] Sending login request...\n");
    http_response_t* response = http_request(
        client, 
        HTTP_POST, 
        full_url, 
        headers, 
        header_count, 
        login_json, 
        strlen(login_json)
    );

    // 5. Handle the response
    if (response->error) {
        printf("[!] Request failed: %s (Error code: %d)\n", 
               response->error, response->error_code);
    } else {
        printf("[+] Response received: HTTP %d\n", response->status_code);
        
        if (response->status_code == 200 || response->status_code == 201) {
            printf("[+] Login successful!\n");
        } else {
            printf("[-] Login failed with status: %d\n", response->status_code);
        }

        // Print response body if available
        if (response->body_len > 0) {
            printf("\nResponse Body:\n%s\n", response->body);
        }
    }

    // 6. Cleanup
    printf("\n[*] Cleaning up resources...\n");
    http_response_free(response);
    http_client_destroy(client);

    printf("--- Session Ended ---\n");
    return 0;
}
