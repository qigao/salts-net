#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Request logging interceptor
static int log_request(http_request_context_t* ctx) {
    printf("[REQUEST] %s %s\n", 
           ctx->method == HTTP_GET ? "GET" :
           ctx->method == HTTP_POST ? "POST" :
           ctx->method == HTTP_PUT ? "PUT" :
           ctx->method == HTTP_DELETE ? "DELETE" : "OTHER",
           ctx->url);
    
    if (ctx->body && ctx->body_len > 0) {
        printf("[REQUEST] Body: %zu bytes\n", ctx->body_len);
    }
    
    return 0;  // Continue with request
}

// Response logging interceptor
static void log_response(http_response_context_t* ctx) {
    if (ctx->response->error) {
        printf("[RESPONSE] Error: %s (code: %d)\n", 
               ctx->response->error, ctx->response->error_code);
    } else {
        printf("[RESPONSE] Status: %d, Body: %zu bytes\n",
               ctx->response->status_code, ctx->response->body_len);
    }
}

// Timing interceptor data
typedef struct {
    clock_t start_time;
} timing_data_t;

// Request timing interceptor
static int timing_request(http_request_context_t* ctx) {
    timing_data_t* data = (timing_data_t*)ctx->user_data;
    data->start_time = clock();
    return 0;
}

// Response timing interceptor
static void timing_response(http_response_context_t* ctx) {
    timing_data_t* data = (timing_data_t*)ctx->user_data;
    clock_t end_time = clock();
    double elapsed = ((double)(end_time - data->start_time)) / CLOCKS_PER_SEC * 1000.0;
    printf("[TIMING] Request took %.2f ms\n", elapsed);
}

// Custom header interceptor
static int add_custom_headers(http_request_context_t* ctx) {
    printf("[INTERCEPTOR] Adding custom headers (simulated)\n");
    // In a real implementation, you might modify the request here
    return 0;
}

// Response validation interceptor
static void validate_response(http_response_context_t* ctx) {
    if (!ctx->response->error && ctx->response->status_code >= 400) {
        printf("[VALIDATOR] Warning: HTTP error status %d\n", 
               ctx->response->status_code);
    }
}

int main(void) {
    printf("=== Request/Response Interceptors Example ===\n\n");
    
    http_client_t* client = http_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create HTTP client\n");
        return 1;
    }

    printf("1. Basic logging interceptors...\n\n");
    
    // Add logging interceptors
    http_client_add_request_interceptor(client, log_request, NULL);
    http_client_add_response_interceptor(client, log_response, NULL);
    
    // Make a request
    http_response_t* response = http_get(client, "https://httpbin.org/get");
    http_response_free(response);
    
    printf("\n2. Adding timing interceptors...\n\n");
    
    // Add timing interceptors
    timing_data_t timing_data = {0};
    http_client_add_request_interceptor(client, timing_request, &timing_data);
    http_client_add_response_interceptor(client, timing_response, &timing_data);
    
    // Make another request
    response = http_get(client, "https://httpbin.org/delay/1");
    http_response_free(response);
    
    printf("\n3. Adding validation interceptor...\n\n");
    
    http_client_add_response_interceptor(client, validate_response, NULL);
    
    // Make a request that will return 404
    response = http_get(client, "https://httpbin.org/status/404");
    http_response_free(response);
    
    printf("\n4. Clearing interceptors and making clean request...\n\n");
    
    http_client_clear_interceptors(client);
    
    response = http_get(client, "https://httpbin.org/get");
    if (!response->error) {
        printf("Request completed without interceptors (status: %d)\n", 
               response->status_code);
    }
    http_response_free(response);
    
    printf("\n5. Demonstrating request abortion...\n\n");
    
    // Add an interceptor that aborts requests
    http_client_add_request_interceptor(client, add_custom_headers, NULL);
    
    response = http_get(client, "https://httpbin.org/get");
    if (!response->error) {
        printf("Request succeeded (status: %d)\n", response->status_code);
    }
    http_response_free(response);
    
    // Cleanup
    http_client_destroy(client);
    
    printf("\n Interceptors example complete!\n");
    return 0;
}
