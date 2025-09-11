/**
 * Error Handling Tests  
 * "Good error handling eliminates debugging time"
 */
#include "unity.h"
#include "turbonet.h"
#include <string.h>
#include <uv.h>

static turbo_handle_t test_handle;
static turbo_error_t captured_error;
static int error_callback_called;

static void test_error_callback(turbo_handle_t* handle, const turbo_error_t* error) {
    captured_error = *error;
    error_callback_called = 1;
}

static void setup_error_test(void) {
    memset(&test_handle, 0, sizeof(test_handle));
    memset(&captured_error, 0, sizeof(captured_error));
    error_callback_called = 0;
}

// Test basic error setting and retrieval
void test_turbo_set_error_basic(void) {
    setup_error_test();
    turbo_set_error(&test_handle, UV_ECONNREFUSED, "tcp://127.0.0.1:80", "Connection refused by server");
    
    turbo_error_t retrieved_error;
    int result = turbo_get_last_error(&test_handle, &retrieved_error);
    
    TEST_ASSERT_EQUAL_INT(UV_ECONNREFUSED, result);
    TEST_ASSERT_EQUAL_INT(UV_ECONNREFUSED, retrieved_error.code);
    TEST_ASSERT_EQUAL_STRING("Connection refused", retrieved_error.message);
    TEST_ASSERT_EQUAL_STRING("tcp://127.0.0.1:80", retrieved_error.context);
    TEST_ASSERT_EQUAL_STRING("Connection refused by server", retrieved_error.details);
    TEST_ASSERT_NOT_EQUAL(0, retrieved_error.timestamp);
}

// Test error callback mechanism
void test_turbo_error_callback(void) {
    setup_error_test();
    turbo_set_error_callback(&test_handle, test_error_callback);
    
    turbo_set_error(&test_handle, UV_EAI_NONAME, "dns://example.com", "Domain not found");
    
    TEST_ASSERT_EQUAL_INT(1, error_callback_called);
    TEST_ASSERT_EQUAL_INT(UV_EAI_NONAME, captured_error.code);
    TEST_ASSERT_EQUAL_STRING("dns://example.com", captured_error.context);
}

// Test error clearing
void test_turbo_clear_error(void) {
    setup_error_test();
    turbo_set_error(&test_handle, UV_ECONNREFUSED, "test", "test error");
    turbo_clear_error(&test_handle);
    
    turbo_error_t retrieved_error;
    int result = turbo_get_last_error(&test_handle, &retrieved_error);
    
    TEST_ASSERT_EQUAL_INT(0, result);  // No error available, should return 0
}

// Test error string formatting
void test_turbo_error_to_string(void) {
    turbo_error_t error = {
        .code = UV_ECONNREFUSED,
        .message = "Connection refused",
        .context = "tcp://127.0.0.1:8080", 
        .details = "Server not accepting connections",
        .timestamp = 1234567890
    };
    
    char formatted[512];
    int result = turbo_error_to_string(&error, formatted, sizeof(formatted));
    
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_NOT_NULL(strstr(formatted, "Connection refused"));
    TEST_ASSERT_NOT_NULL(strstr(formatted, "tcp://127.0.0.1:8080"));
}

// Test NULL parameter handling
void test_turbo_error_null_params(void) {
    setup_error_test();
    turbo_set_error(NULL, UV_EINVAL, "test", "test");  // Should not crash
    
    int result = turbo_get_last_error(NULL, &captured_error);
    TEST_ASSERT_EQUAL_INT(UV_EINVAL, result);
    
    result = turbo_get_last_error(&test_handle, NULL);
    TEST_ASSERT_EQUAL_INT(UV_EINVAL, result);
}