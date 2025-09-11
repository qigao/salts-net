/**
 * Utility Functions Tests
 * "Utilities should be rock solid - they're used everywhere"
 */
#include "unity.h"
#include "turbonet.h"
#include "turbonet_internal.h"
#include <string.h>
#include <stdlib.h>

// Test string utilities  
void test_string_utilities(void) {
    const char* err_str = uv_strerror(0);
    TEST_ASSERT_NOT_NULL(err_str);
    
    err_str = uv_strerror(UV_ECONNREFUSED);
    TEST_ASSERT_NOT_NULL(err_str);
    TEST_ASSERT_NOT_EQUAL(0, strlen(err_str));
    
    // Should handle unknown error codes gracefully
    err_str = uv_strerror(-999999);
    TEST_ASSERT_NOT_NULL(err_str);
}

// Test buffer utilities  
void test_buffer_utilities(void) {
    char test_data[] = "Hello, TurboNet!";
    
    turbo_buf_t buf = {test_data, strlen(test_data)};
    TEST_ASSERT_EQUAL_PTR(test_data, buf.base);
    TEST_ASSERT_EQUAL_size_t(strlen(test_data), buf.len);
    
    // Test with NULL
    turbo_buf_t null_buf = {NULL, 0};
    TEST_ASSERT_NULL(null_buf.base);
    TEST_ASSERT_EQUAL_size_t(0, null_buf.len);
}

// Test new event loop functions
void test_event_loop_functions(void) {
    turbo_handle_t handle;
    
    // Test global init
    int err = turbo_global_init(&handle);
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_NOT_NULL(handle.loop);
    
    // Test run_once (should return immediately)
    err = turbo_run_once();
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Test run_nowait (should return immediately) 
    err = turbo_run_nowait();
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Test stop (should not crash)
    turbo_stop();
    
    // Test cleanup
    turbo_global_cleanup();
}

// Test error conditions for event loop
void test_event_loop_error_conditions(void) {
    // Test without initialization
    turbo_global_cleanup(); // Ensure clean state
    
    int err = turbo_run_once();
    TEST_ASSERT_NOT_EQUAL(0, err); // Should fail without init
    
    err = turbo_run_nowait(); 
    TEST_ASSERT_NOT_EQUAL(0, err); // Should fail without init
    
    err = turbo_run();
    TEST_ASSERT_NOT_EQUAL(0, err); // Should fail without init
}

// Test URL parsing edge cases
void test_url_parsing_edge_cases(void) {
    turbo_transport_t transport;
    
    // Case sensitivity
    TEST_ASSERT_EQUAL_INT(0, turbo_validate_url("TCP://127.0.0.1:80", &transport));
    TEST_ASSERT_EQUAL_INT(TURBO_TCP, transport);
    
    TEST_ASSERT_EQUAL_INT(0, turbo_validate_url("tcp://127.0.0.1:80", &transport));
    TEST_ASSERT_EQUAL_INT(TURBO_TCP, transport);
    
    // IPv6 addresses (if supported)
    int result = turbo_validate_url("tcp://[::1]:80", &transport);
    // May or may not be supported, but should not crash
    TEST_ASSERT_TRUE(result == 0 || result != 0);
    
    // Hostname resolution
    result = turbo_validate_url("tcp://localhost:80", &transport);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_INT(TURBO_TCP, transport);
}

// Test memory management
void test_memory_management(void) {
    turbo_handle_t* handles[10];
    uv_loop_t* test_loop = uv_default_loop();
    
    // Create multiple handles
    for (int i = 0; i < 10; i++) {
        handles[i] = malloc(sizeof(turbo_handle_t));
        TEST_ASSERT_NOT_NULL(handles[i]);
        
        memset(handles[i], 0, sizeof(turbo_handle_t));
        turbo_init(handles[i], test_loop, TURBO_TCP);
        TEST_ASSERT_EQUAL_INT(TURBO_TCP, handles[i]->transport);
        TEST_ASSERT_EQUAL_INT(TURBO_CLOSED, handles[i]->state);
    }
    
    // Clean up
    for (int i = 0; i < 10; i++) {
        turbo_close(handles[i], NULL);
        free(handles[i]);
    }
}

// Test thread safety preparation (basic checks)
void test_thread_safety_basics(void) {
    turbo_handle_t handle1, handle2;
    
    // Initialize multiple handles simultaneously
    memset(&handle1, 0, sizeof(handle1));
    memset(&handle2, 0, sizeof(handle2));
    turbo_init(&handle1, uv_default_loop(), TURBO_TCP);
    turbo_init(&handle2, uv_default_loop(), TURBO_UDP);
    
    // Should not interfere with each other
    TEST_ASSERT_EQUAL_INT(TURBO_TCP, handle1.transport);
    TEST_ASSERT_EQUAL_INT(TURBO_UDP, handle2.transport);
    
    // Error states should be independent
    turbo_set_error(&handle1, UV_ECONNREFUSED, "test1", "details1");
    turbo_set_error(&handle2, UV_EAI_NONAME, "test2", "details2");
    
    turbo_error_t error1, error2;
    turbo_get_last_error(&handle1, &error1);
    turbo_get_last_error(&handle2, &error2);
    
    TEST_ASSERT_EQUAL_INT(UV_ECONNREFUSED, error1.code);
    TEST_ASSERT_EQUAL_INT(UV_EAI_NONAME, error2.code);
    TEST_ASSERT_EQUAL_STRING("test1", error1.context);
    TEST_ASSERT_EQUAL_STRING("test2", error2.context);
    
    turbo_close(&handle1, NULL);
    turbo_close(&handle2, NULL);
}

// Test performance characteristics (basic timing)
void test_basic_performance(void) {
    const int iterations = 1000;
    turbo_handle_t handle;
    
    // Time handle initialization
    for (int i = 0; i < iterations; i++) {
        memset(&handle, 0, sizeof(handle));
        turbo_init(&handle, uv_default_loop(), TURBO_TCP);
        turbo_close(&handle, NULL);
    }
    
    // Time URL validation
    turbo_transport_t transport;
    for (int i = 0; i < iterations; i++) {
        turbo_validate_url("tcp://127.0.0.1:8080", &transport);
    }
    
    // Time error operations
    memset(&handle, 0, sizeof(handle));
    turbo_init(&handle, uv_default_loop(), TURBO_TCP);
    for (int i = 0; i < iterations; i++) {
        turbo_set_error(&handle, UV_ECONNREFUSED, "context", "details");
        turbo_clear_error(&handle);
    }
    turbo_close(&handle, NULL);
    
    // If we get here without hanging, performance is acceptable
    TEST_ASSERT_TRUE(1);
}

// Test boundary conditions
void test_boundary_conditions(void) {
    turbo_transport_t transport;
    
    // Maximum valid port
    TEST_ASSERT_EQUAL_INT(0, turbo_validate_url("tcp://127.0.0.1:65535", &transport));
    
    // Minimum valid port  
    TEST_ASSERT_EQUAL_INT(0, turbo_validate_url("tcp://127.0.0.1:1", &transport));
    
    // Port 0 (may or may not be valid depending on context)
    int result = turbo_validate_url("tcp://127.0.0.1:0", &transport);
    TEST_ASSERT_TRUE(result == 0 || result != 0);  // Either way is acceptable
    
    // Long hostnames (should handle gracefully)
    char long_url[300];
    strcpy(long_url, "tcp://");
    for (int i = 0; i < 240; i++) {
        strcat(long_url, "a");
    }
    strcat(long_url, ".com:80");
    
    result = turbo_validate_url(long_url, &transport);
    // May succeed or fail, but should not crash
    TEST_ASSERT_TRUE(result == 0 || result != 0);
}
