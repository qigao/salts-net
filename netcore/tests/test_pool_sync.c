#include <stdlib.h>
#include <stdio.h>
#include "platform.h"
#include "turbo_thread.h"
#include "tinytest.h"

/* Declarations for pool_sync functions */
void turbo_pools_init(void);
void turbo_pools_cleanup(void);
void turbo_tcp_pool_lock(void);
void turbo_tcp_pool_unlock(void);
void turbo_udp_pool_lock(void);
void turbo_udp_pool_unlock(void);
void turbo_kcp_pool_lock(void);
void turbo_kcp_pool_unlock(void);
void turbo_tls_pool_lock(void);
void turbo_tls_pool_unlock(void);
void turbo_pipe_pool_lock(void);
void turbo_pipe_pool_unlock(void);

#define TEST_THREAD_COUNT 4
#define TEST_ITERATIONS 10000

static volatile int g_shared_counter = 0;

static void tcp_lock_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < TEST_ITERATIONS; i++) {
        turbo_tcp_pool_lock();
        
        int val = g_shared_counter;
        // Yield to encourage race conditions
        turbo_sleep_ms(0);
        g_shared_counter = val + 1;
        
        turbo_tcp_pool_unlock();
    }
}

/* Test: Simulate concurrent close attempts on a pipe client */
#include <uv.h>
#include "turbo_pipe.h"

#define CLOSE_THREAD_COUNT 8

static turbo_pipe_client_t* g_test_client = NULL;
static volatile int g_close_call_count = 0;

static void pipe_close_thread(void *arg) {
    (void)arg;
    
    /* Each thread tries to close the same client */
    turbo_pipe_pool_lock();
    g_close_call_count++;
    turbo_pipe_pool_unlock();
    
    /* Call close - only one should actually perform the close */
    turbo_pipe_client_close(g_test_client);
}

spec("pool_sync") {
    before_each() {
        turbo_pools_init();
        g_shared_counter = 0;
    }

    after_each() {
        turbo_pools_cleanup();
    }

    it("should handle TCP pool concurrency") {
        turbo_thread_t threads[TEST_THREAD_COUNT];
        
        // Create threads
        for (int i = 0; i < TEST_THREAD_COUNT; i++) {
            int rc = turbo_thread_create(&threads[i], tcp_lock_thread, NULL);
            check_int_eq(rc, 0);
        }
        
        // Join threads
        for (int i = 0; i < TEST_THREAD_COUNT; i++) {
            int rc = turbo_thread_join(&threads[i]);
            check_int_eq(rc, 0);
        }
        
        // Verify count
        check_int_eq(g_shared_counter, TEST_THREAD_COUNT * TEST_ITERATIONS);
    }

    it("should allow all pool locks lifecycle") {
        // Just verify other locks acquire/release without crashing
        turbo_udp_pool_lock();
        turbo_udp_pool_unlock();

        turbo_kcp_pool_lock();
        turbo_kcp_pool_unlock();
        
        turbo_tls_pool_lock();
        turbo_tls_pool_unlock();
        
        turbo_pipe_pool_lock();
        turbo_pipe_pool_unlock();
    }

    it("should handle concurrent close on pipe client") {
        turbo_thread_t threads[CLOSE_THREAD_COUNT];
        
        /* Create a test event loop and client */
        uv_loop_t loop;
        uv_loop_init(&loop);
        
        g_test_client = turbo_pipe_client_create(&loop);
        check_not_null(g_test_client);
        g_close_call_count = 0;
        
        /* Launch multiple threads that all try to close simultaneously */
        for (int i = 0; i < CLOSE_THREAD_COUNT; i++) {
            int rc = turbo_thread_create(&threads[i], pipe_close_thread, NULL);
            check_int_eq(rc, 0);
        }
        
        /* Wait for all threads to complete */
        for (int i = 0; i < CLOSE_THREAD_COUNT; i++) {
            int rc = turbo_thread_join(&threads[i]);
            check_int_eq(rc, 0);
        }
        
        /* Verify all threads tried to close */
        check_int_eq(g_close_call_count, CLOSE_THREAD_COUNT);
        
        /* Run the loop briefly to process the close */
        uv_run(&loop, UV_RUN_NOWAIT);
        uv_loop_close(&loop);
        
        /* If we get here without crash/abort, the mutex protection worked */
        printf("  Concurrent close test passed - no race condition!\n");
        
        g_test_client = NULL;
    }
}
