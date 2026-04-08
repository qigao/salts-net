#include "platform.h"
#include "turbo_thread.h"
#include "tinytest.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_THREAD_COUNT 4
#define TEST_ITERATIONS 10000

static turbo_mutex_t mutex;
static volatile int shared_counter = 0;

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

spec("Mutex Tests") {
  before_each() {
    turbo_mutex_init(&mutex);
    shared_counter = 0;
  }

  after_each() { turbo_mutex_destroy(&mutex); }

  it("should perform basic lock/unlock") {
    // Basic lock/unlock on single thread shouldn't crash
    turbo_mutex_lock(&mutex);
    shared_counter = 42;
    turbo_mutex_unlock(&mutex);

    check_int_eq(shared_counter, 42);
  }

  it("should handle concurrent access") {
    turbo_thread_t threads[TEST_THREAD_COUNT];

    // Start threads
    for (int i = 0; i < TEST_THREAD_COUNT; i++) {
      int rc = turbo_thread_create(&threads[i], mutex_test_thread, NULL);
      check_int_eq(rc, 0);
    }

    // Join threads
    for (int i = 0; i < TEST_THREAD_COUNT; i++) {
      int rc = turbo_thread_join(&threads[i]);
      check_int_eq(rc, 0);
    }

    // Verify count is exactly threads * iterations
    // If mutex was broken, race conditions would make this value smaller
    check_int_eq(shared_counter, TEST_THREAD_COUNT * TEST_ITERATIONS);
  }
}
