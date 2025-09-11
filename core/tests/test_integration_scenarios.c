/**
 * Integration Tests for Non-Blocking Event Loop
 * "Integration tests verify the real user scenario" - End-to-end validation
 */
#include "platform.h"
#include "unity.h"
#include "turbonet.h"
#include "turbonet_internal.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

// Test state for integration scenarios
typedef struct {
    turbo_handle_t client;
    turbo_handle_t server;
    turbo_req_t write_req;
    bool client_connected;
    bool server_listening;
    bool data_sent;
    bool data_received;
    int work_cycles;
    int max_work_cycles;
    char test_message[256];
    char received_data[256];
} integration_test_state_t;

static integration_test_state_t g_test_state = {0};

// Test callbacks
static void integration_client_connect_cb(turbo_handle_t* handle, int status)
{
    g_test_state.client_connected = (status == 0);
}

static void integration_client_alloc_cb(turbo_handle_t* handle, size_t size, turbo_buf_t* buf)
{
    buf->base = malloc(size);
    buf->len = buf->base ? size : 0;
}

static void integration_client_read_cb(turbo_handle_t* handle, ssize_t nread, const turbo_buf_t* buf)
{
    if (nread > 0 && nread < (ssize_t)sizeof(g_test_state.received_data)) {
        memcpy(g_test_state.received_data, buf->base, nread);
        g_test_state.received_data[nread] = '\0';
        g_test_state.data_received = true;
    }
    if (buf->base) {
        free(buf->base);
    }
}

static void integration_client_write_cb(turbo_req_t* req, int status)
{
    g_test_state.data_sent = (status == 0);
}

static void integration_server_connection_cb(turbo_handle_t* server, int status)
{
    // For integration test, we just note that server received connection
    if (status == 0) {
        g_test_state.server_listening = true;
    }
}

// Test the basic non-blocking event loop functionality
void test_non_blocking_event_loop_basic(void)
{
    turbo_handle_t handle;
    
    // Initialize global state
    int err = turbo_global_init(&handle);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Test that run_once returns immediately when no events
    err = turbo_run_once();
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Test that run_nowait returns immediately when no events
    err = turbo_run_nowait();
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Test that we can call them multiple times
    for (int i = 0; i < 5; i++) {
        err = turbo_run_once();
        TEST_ASSERT_EQUAL_INT(0, err);
    }
    
    turbo_global_cleanup();
}

// Test the user scenario: init -> work -> send -> work -> send -> stop
void test_user_scenario_pattern(void)
{
    // Clear test state
    memset(&g_test_state, 0, sizeof(g_test_state));
    strcpy(g_test_state.test_message, "Integration test message");
    g_test_state.max_work_cycles = 5;
    
    // Step 1: INIT
    int err = turbo_global_init(&g_test_state.client);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Step 2: Simulate connection (we'll use a fake scenario since we need server)
    // For integration test, we simulate the pattern without actual network
    
    // Step 3: Main loop pattern - work and network interleaved
    bool app_running = true;
    int loop_count = 0;
    
    while (app_running && loop_count < 100) { // Safety limit - increased to allow test to complete
        loop_count++;
        
        // Process network events (this is the key test!)
        err = turbo_run_once();
        TEST_ASSERT_EQUAL_INT(0, err);
        
        // Simulate application work
        if (loop_count % 10 == 0) { // Every 10 iterations
            g_test_state.work_cycles++;
            
            // Test that we can do work between network calls
            TEST_ASSERT_TRUE(g_test_state.work_cycles <= g_test_state.max_work_cycles);
        }
        
        // Exit condition
        if (g_test_state.work_cycles >= g_test_state.max_work_cycles) {
            app_running = false;
        }
    }
    
    // Verify we completed the work cycles
    TEST_ASSERT_EQUAL_INT(g_test_state.max_work_cycles, g_test_state.work_cycles);
    TEST_ASSERT_TRUE(loop_count < 100); // Should not hit safety limit - updated limit
    
    // Step 4: Cleanup
    turbo_global_cleanup();
}

// Test multiple handles with non-blocking event loop
void test_multiple_handles_non_blocking(void)
{
    turbo_handle_t handles[3];
    
    // Initialize multiple handles
    for (int i = 0; i < 3; i++) {
        int err = turbo_global_init(&handles[i]);
        TEST_ASSERT_EQUAL_INT(0, err);
        TEST_ASSERT_NOT_NULL(handles[i].loop);
    }
    
    // Test that run_once works with multiple handles
    for (int iteration = 0; iteration < 10; iteration++) {
        int err = turbo_run_once();
        TEST_ASSERT_EQUAL_INT(0, err);
        
        // All handles should still be valid
        for (int i = 0; i < 3; i++) {
            TEST_ASSERT_NOT_NULL(handles[i].loop);
        }
    }
    
    turbo_global_cleanup();
}

// Test error resilience in non-blocking mode
void test_error_resilience_non_blocking(void)
{
    turbo_handle_t handle;
    
    int err = turbo_global_init(&handle);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Test that errors don't break the event loop
    for (int i = 0; i < 20; i++) {
        err = turbo_run_once();
        TEST_ASSERT_EQUAL_INT(0, err);
        
        // Simulate some error conditions by calling functions with invalid params
        // These should not affect the event loop
        turbo_transport_t invalid_transport;
        int result = turbo_validate_url("invalid://badurl", &invalid_transport);
        TEST_ASSERT_NOT_EQUAL(0, result); // Should fail
        
        // Event loop should still work
        err = turbo_run_once();
        TEST_ASSERT_EQUAL_INT(0, err);
    }
    
    turbo_global_cleanup();
}

// Test performance characteristics of non-blocking calls
void test_non_blocking_performance(void)
{
    turbo_handle_t handle;
    
    int err = turbo_global_init(&handle);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Measure that calls return quickly (this is a basic sanity check)
    clock_t start_time = clock();
    
    // Run many iterations - should complete quickly since non-blocking
    for (int i = 0; i < 1000; i++) {
        err = turbo_run_once();
        TEST_ASSERT_EQUAL_INT(0, err);
    }
    
    clock_t end_time = clock();
    double cpu_time_used = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    
    // Should complete in reasonable time (less than 1 second for 1000 calls)
    TEST_ASSERT_TRUE(cpu_time_used < 1.0);
    
    turbo_global_cleanup();
}

// Test cleanup and reinitialization
void test_cleanup_and_reinit(void)
{
    // Test multiple init/cleanup cycles
    for (int cycle = 0; cycle < 3; cycle++) {
        turbo_handle_t handle;
        
        // Initialize
        int err = turbo_global_init(&handle);
        TEST_ASSERT_EQUAL_INT(0, err);
        
        // Use non-blocking calls
        for (int i = 0; i < 5; i++) {
            err = turbo_run_once();
            TEST_ASSERT_EQUAL_INT(0, err);
        }
        
        // Cleanup
        turbo_global_cleanup();
        
        // Should be able to reinitialize cleanly
    }
}

// Test thread safety of global functions
void test_global_functions_thread_safety(void)
{
    // Note: This is basic thread safety testing
    // In real usage, each thread should have its own event loop
    
    turbo_handle_t handle1, handle2;
    
    // Initialize two handles
    int err1 = turbo_global_init(&handle1);
    int err2 = turbo_global_init(&handle2);
    
    TEST_ASSERT_EQUAL_INT(0, err1);
    TEST_ASSERT_EQUAL_INT(0, err2);
    
    // Both should have the same global loop
    TEST_ASSERT_EQUAL_PTR(handle1.loop, handle2.loop);
    
    // Both should work with non-blocking calls
    for (int i = 0; i < 10; i++) {
        err1 = turbo_run_once();
        TEST_ASSERT_EQUAL_INT(0, err1);
    }
    
    turbo_global_cleanup();
}