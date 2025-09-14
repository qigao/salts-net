#include "http_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int request_called = 0;
static int response_called = 0;

static int test_request_interceptor(http_request_context_t* ctx) {
    request_called++;
    assert(ctx != NULL);
    assert(ctx->url != NULL);
    return 0;  // Continue
}

static void test_response_interceptor(http_response_context_t* ctx) {
    response_called++;
    assert(ctx != NULL);
    assert(ctx->response != NULL);
}

static void test_basic_interceptors(void) {
    printf("Testing basic interceptors...\n");
    
    http_client_t* client = http_client_create();
    assert(client != NULL);
    http_client_set_timeout(client, 3000);
    
    request_called = 0;
    response_called = 0;
    
    // Add interceptors
    http_client_add_request_interceptor(client, test_request_interceptor, NULL);
    http_client_add_response_interceptor(client, test_response_interceptor, NULL);
    
    // Make request
    http_response_t* response = http_get(client, "https://httpbin.org/get");
    
    if (response->error) {
        printf("  Skipping test (network error): %s\n", response->error);
        http_response_free(response);
        http_client_destroy(client);
        return;
    }
    
    // Verify interceptors were called
    assert(request_called == 1);
    assert(response_called == 1);
    
    http_response_free(response);
    http_client_destroy(client);
    
    printf("   Basic interceptors work correctly\n");
}

static int abort_request_interceptor(http_request_context_t* ctx) {
    return 1;  // Abort request
}

static void test_request_abortion(void) {
    printf("Testing request abortion...\n");
    
    http_client_t* client = http_client_create();
    assert(client != NULL);
    
    // Add interceptor that aborts
    http_client_add_request_interceptor(client, abort_request_interceptor, NULL);
    
    // Make request
    http_response_t* response = http_get(client, "https://httpbin.org/get");
    
    // Should have error
    assert(response->error != NULL);
    assert(strstr(response->error, "aborted") != NULL);
    
    http_response_free(response);
    http_client_destroy(client);
    
    printf("   Request abortion works correctly\n");
}

static int user_data_value = 0;

static int check_user_data_request(http_request_context_t* ctx) {
    int* value = (int*)ctx->user_data;
    assert(value != NULL);
    assert(*value == 42);
    user_data_value = *value;
    return 0;
}

static void check_user_data_response(http_response_context_t* ctx) {
    int* value = (int*)ctx->user_data;
    assert(value != NULL);
    assert(*value == 99);
    user_data_value = *value;
}

static void test_user_data(void) {
    printf("Testing interceptor user data...\n");
    
    http_client_t* client = http_client_create();
    assert(client != NULL);
    http_client_set_timeout(client, 3000);
    
    int request_data = 42;
    int response_data = 99;
    
    http_client_add_request_interceptor(client, check_user_data_request, &request_data);
    http_client_add_response_interceptor(client, check_user_data_response, &response_data);
    
    user_data_value = 0;
    
    http_response_t* response = http_get(client, "https://httpbin.org/get");
    
    if (response->error) {
        printf("  Skipping test (network error): %s\n", response->error);
        http_response_free(response);
        http_client_destroy(client);
        return;
    }
    
    // Verify user data was passed correctly
    assert(user_data_value == 99);  // Last one set
    
    http_response_free(response);
    http_client_destroy(client);
    
    printf("   User data works correctly\n");
}

static void test_multiple_interceptors(void) {
    printf("Testing multiple interceptors...\n");
    
    http_client_t* client = http_client_create();
    assert(client != NULL);
    http_client_set_timeout(client, 3000);
    
    request_called = 0;
    response_called = 0;
    
    // Add multiple interceptors
    http_client_add_request_interceptor(client, test_request_interceptor, NULL);
    http_client_add_request_interceptor(client, test_request_interceptor, NULL);
    http_client_add_response_interceptor(client, test_response_interceptor, NULL);
    http_client_add_response_interceptor(client, test_response_interceptor, NULL);
    
    http_response_t* response = http_get(client, "https://httpbin.org/get");
    
    if (response->error) {
        printf("  Skipping test (network error): %s\n", response->error);
        http_response_free(response);
        http_client_destroy(client);
        return;
    }
    
    // Both interceptors should have been called
    assert(request_called == 2);
    assert(response_called == 2);
    
    http_response_free(response);
    http_client_destroy(client);
    
    printf("   Multiple interceptors work correctly\n");
}

static void test_clear_interceptors(void) {
    printf("Testing clear interceptors...\n");
    
    http_client_t* client = http_client_create();
    assert(client != NULL);
    http_client_set_timeout(client, 3000);
    
    request_called = 0;
    response_called = 0;
    
    // Add interceptors
    http_client_add_request_interceptor(client, test_request_interceptor, NULL);
    http_client_add_response_interceptor(client, test_response_interceptor, NULL);
    
    // Clear them
    http_client_clear_interceptors(client);
    
    // Make request
    http_response_t* response = http_get(client, "https://httpbin.org/get");
    
    if (response->error) {
        printf("  Skipping test (network error): %s\n", response->error);
        http_response_free(response);
        http_client_destroy(client);
        return;
    }
    
    // Interceptors should not have been called
    assert(request_called == 0);
    assert(response_called == 0);
    
    http_response_free(response);
    http_client_destroy(client);
    
    printf("   Clear interceptors works correctly\n");
}

int main(void) {
    printf("\n=== HTTP Client Interceptors Tests ===\n\n");
    
    test_basic_interceptors();
    test_request_abortion();
    test_user_data();
    test_multiple_interceptors();
    test_clear_interceptors();
    
    printf("\n All interceptor tests passed!\n");
    return 0;
}
