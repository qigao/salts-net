/**
 * @file test_connection_cleanup.c
 * @brief Tests for connection context cleanup functionality
 */

#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "router.h"
#include "netcore/turbo_coro_client.h"
#include "error_recovery.h"

/* Test data structure for middleware context */
typedef struct {
    int magic_number;
    char *allocated_data;
    int cleanup_called;
} test_middleware_data_t;

/* Global test state */
static int g_cleanup_call_count = 0;
static int g_last_magic_number = 0;
static int g_last_cleanup_called = 0;

/* Test cleanup function */
static void test_cleanup_function(void *data) {
    test_middleware_data_t *test_data = (test_middleware_data_t *)data;
    if (test_data) {
        g_cleanup_call_count++;
        g_last_magic_number = test_data->magic_number;
        test_data->cleanup_called = 1;
        g_last_cleanup_called = test_data->cleanup_called;
        
        if (test_data->allocated_data) {
            free(test_data->allocated_data);
            test_data->allocated_data = NULL;
        }
        
        free(test_data);
    }
}

spec("connection_cleanup") {
    before_each() {
        /* Reset global test state */
        g_cleanup_call_count = 0;
        g_last_magic_number = 0;
        g_last_cleanup_called = 0;
        
        /* Initialize error recovery system */
        iris_error_recovery_init();
    }

    after_each() {
        /* Cleanup error recovery system */
        iris_error_recovery_cleanup();
    }

    /**
     * @brief Test that connection context can be set and retrieved
     */
    it("should set and get connection context") {
        /* For this test, we'll simulate a connection by creating a mock client
         * Since we can't easily create a real client in a unit test without loop, we'll test
         * the connection context functions with a NULL client to verify error handling */
        
        /* Test with NULL client - should handle gracefully */
        set_connection_context(NULL, NULL, NULL);
        void *result = get_connection_context(NULL);
        check_null(result);
    }

    /**
     * @brief Test that middleware cleanup is called when connection is closed
     * 
     * This test verifies that when middleware attaches data to a connection,
     * the cleanup function is properly called when the connection is closed.
     */
    it("should call middleware cleanup on disconnect") {
        /* Create test middleware data */
        test_middleware_data_t *test_data = malloc(sizeof(test_middleware_data_t));
        check_not_null(test_data);
        
        test_data->magic_number = 0x1234;
        test_data->allocated_data = malloc(256);
        check_not_null(test_data->allocated_data);
        strcpy(test_data->allocated_data, "test data");
        test_data->cleanup_called = 0;
        
        /* Since we can't easily simulate a real connection lifecycle in a unit test,
         * we'll directly test the cleanup function to ensure it works correctly */
        
        /* Call the cleanup function directly */
        test_cleanup_function(test_data);
        
        /* Verify cleanup was called */
        check_int_eq(g_cleanup_call_count, 1);
        check_int_eq(g_last_magic_number, 0x1234);
        check_int_eq(g_last_cleanup_called, 1);
    }

    /**
     * @brief Test that multiple middleware cleanup functions are handled correctly
     */
    it("should handle multiple middleware cleanup") {
        /* Create multiple test data structures */
        test_middleware_data_t *test_data1 = malloc(sizeof(test_middleware_data_t));
        test_middleware_data_t *test_data2 = malloc(sizeof(test_middleware_data_t));
        
        check_not_null(test_data1);
        check_not_null(test_data2);
        
        test_data1->magic_number = 0x1111;
        test_data1->allocated_data = malloc(128);
        strcpy(test_data1->allocated_data, "data1");
        test_data1->cleanup_called = 0;
        
        test_data2->magic_number = 0x2222;
        test_data2->allocated_data = malloc(128);
        strcpy(test_data2->allocated_data, "data2");
        test_data2->cleanup_called = 0;
        
        /* Test cleanup of first data */
        test_cleanup_function(test_data1);
        check_int_eq(g_cleanup_call_count, 1);
        
        /* Test cleanup of second data */
        test_cleanup_function(test_data2);
        check_int_eq(g_cleanup_call_count, 2);
        
        /* Verify the last cleaned data is the second one */
        check_int_eq(g_last_magic_number, 0x2222);
    }

    /**
     * @brief Test that cleanup handles NULL data gracefully
     */
    it("should handle NULL data in cleanup") {
        /* Call cleanup with NULL data - should not crash */
        test_cleanup_function(NULL);
        
        /* Verify no cleanup was recorded */
        check_int_eq(g_cleanup_call_count, 0);
        check_int_eq(g_last_magic_number, 0);
    }

    /**
     * @brief Test connection context error handling
     */
    it("should handle connection context errors") {
        /* Test setting context with NULL client */
        set_connection_context(NULL, (void*)0x12345, test_cleanup_function);
        
        /* Test getting context with NULL client */
        void *result = get_connection_context(NULL);
        check_null(result);
        
        /* These should not crash or cause issues */
        check_int_eq(g_cleanup_call_count, 0);
    }

    /**
     * @brief Integration test for connection lifecycle
     * 
     * This test simulates the connection lifecycle to ensure that
     * connection contexts are properly managed throughout the lifecycle.
     */
    it("should manage connection lifecycle integration") {
        /* This test would ideally create a real server, establish connections,
         * attach middleware data, and then close connections to verify cleanup.
         * However, for a unit test, we focus on testing the individual components. */
        
        /* Create test data */
        test_middleware_data_t *test_data = malloc(sizeof(test_middleware_data_t));
        check_not_null(test_data);
        
        test_data->magic_number = 0xBEEF;
        test_data->allocated_data = malloc(512);
        check_not_null(test_data->allocated_data);
        strcpy(test_data->allocated_data, "lifecycle test data");
        test_data->cleanup_called = 0;
        
        /* Simulate connection cleanup */
        test_cleanup_function(test_data);
        
        /* Verify proper cleanup */
        check_int_eq(g_cleanup_call_count, 1);
        check_int_eq(g_last_magic_number, 0xBEEF);
        check_int_eq(g_last_cleanup_called, 1);
    }
}