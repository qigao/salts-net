#include <stdio.h>
#include <stdlib.h>
#include "platform.h"
#include "unity.h"
#include "turbo_atomic.h"

#define TEST_THREAD_COUNT 4
#define TEST_ITERATIONS 10000

static turbo_mutex_t mutex;
static volatile int shared_counter = 0;

void setUp(void) {
    turbo_mutex_init(&mutex);
    shared_counter = 0;
}

void tearDown(void) {
    turbo_mutex_destroy(&mutex);
}

// Thread function that increments a shared counter protected by mutex
static void mutex_test_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < TEST_ITERATIONS; i++) {
        turbo_mutex_lock(&mutex);
        
        // Critical section
        int val = shared_counter;
        // Small delay to encourage race if lock is broken
        // But we don't want to slow down test too much
        // turbo_sleep_ms(0) yields
        
        shared_counter = val + 1;
        
        turbo_mutex_unlock(&mutex);
    }
}

void test_mutex_basic(void) {
    // Basic lock/unlock on single thread shouldn't crash
    turbo_mutex_lock(&mutex);
    shared_counter = 42;
    turbo_mutex_unlock(&mutex);
    
    TEST_ASSERT_EQUAL(42, shared_counter);
}

void test_mutex_concurrency(void) {
    turbo_thread_t threads[TEST_THREAD_COUNT];
    
    // Start threads
    for (int i = 0; i < TEST_THREAD_COUNT; i++) {
        int rc = turbo_thread_create(&threads[i], mutex_test_thread, NULL);
        TEST_ASSERT_EQUAL(0, rc);
    }
    
    // Join threads
    for (int i = 0; i < TEST_THREAD_COUNT; i++) {
        int rc = turbo_thread_join(&threads[i]);
        TEST_ASSERT_EQUAL(0, rc);
    }
    
    // Verify count is exactly threads * iterations
    // If mutex was broken, race conditions would make this value smaller
    TEST_ASSERT_EQUAL(TEST_THREAD_COUNT * TEST_ITERATIONS, shared_counter);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mutex_basic);
    RUN_TEST(test_mutex_concurrency);
    return UNITY_END();
}
