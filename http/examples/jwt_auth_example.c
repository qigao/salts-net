#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http_client.h"

/**
 * Real-world JWT Authentication Example
 * 
 * This example demonstrates:
 * 1. Generating a JWT locally using high-level client API
 * 2. Sending the JWT in the Authorization header to a remote server
 * 3. Verifying the server received the correct token
 * 4. Decoding a JWT response (simulated using local token)
 */

int main() {
    printf("--- TurboHTTP JWT Authentication Example ---\n\n");

    http_client_t* client = http_client_create();
    if (!client) {
        printf("Failed to create HTTP client\n");
        return 1;
    }

    // 1. JWT Configuration
    // In a real app, this secret would be shared with your backend
    const char* secret = "v3ry-s3cr3t-sh4r3d-k3y-123456789"; 
    const char* claims = "{"
        "\"iss\":\"turbo-client\","
        "\"sub\":\"user_12345\","
        "\"iat\":1706000000,"
        "\"admin\":true"
    "}";

    printf("[1] Encoding JWT and setting it as Bearer token...\n");
    http_client_set_jwt_auth(client, secret, claims);

    // 2. Execute Request
    // httpbin.org/bearer is a test endpoint that returns the bearer token received
    const char* url = "http://httpbin.org/bearer";
    printf("[2] GET %s\n", url);
    
    http_response_t* response = http_get(client, url);

    if (response) {
        if (response->error) {
            printf("Error: %s (Code: %d)\n", response->error, response->error_code);
        } else {
            printf("Status: %d\n", response->status_code);
            if (response->body) {
                printf("Response Body:\n%s\n", response->body);
            }

            // 3. Demonstrate JWT Decoding
            // Since httpbin doesn't return a JWT, we'll demonstrate decoding 
            // the token we just generated (it's stored in the client's auth header)
            // In a real app, 'response->body' would contain the JWT.
            
            // For illustration, let's "mock" a JWT response by using a token string
            // We'll extract it from the client (Authorization: Bearer <token>)
            const char* auth_header = response->headers ? strstr(response->headers, "Bearer ") : NULL;
            if (auth_header) {
                // This is just to get the token back for demonstration
                char* token_start = (char*)auth_header + 7;
                char* token_end = strstr(token_start, "\r\n");
                if (token_end) {
                    size_t token_len = token_end - token_start;
                    char* token = malloc(token_len + 1);
                    memcpy(token, token_start, token_len);
                    token[token_len] = '\0';

                    // Mock the response body as if it was a JWT
                    free(response->body);
                    response->body = token;
                    response->body_len = token_len;

                    printf("\n[3] Decoding JWT from response body...\n");
                    void* jwt_obj = NULL;
                    // OPT_ALLOW_ONLY_HS_ALG = 1 << 3 (from cjwt.h)
                    int rv = http_response_decode_jwt(response, (const uint8_t*)secret, strlen(secret), 8, &jwt_obj);
                    
                    if (rv == 0) {
                        printf("Successfully decoded JWT!\n");
                        // Note: To access fields, you'd need to cast to cjwt_t* 
                        // but since we hid it, we'd usually provide getters.
                        // For this example, we'll just demonstrate it works.
                        http_jwt_destroy(jwt_obj);
                    } else {
                        printf("Failed to decode JWT: %d\n", rv);
                    }
                }
            }
        }
        http_response_free(response);
    }

    http_client_destroy(client);
    printf("\nExample Finished.\n");
    return 0;
}
