#include "platform.h"
#include "arena_buffer.h"
#include "stats.h"
#include "tinytest.h"
#include "turbo_atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * This test reproduces race conditions in the arena buffer recycling mechanism.
 * Since turbo_arena_t is NOT thread-safe by default, this test is expected to
 * fail or crash if run without external synchronization or internal locks.
 *
 * It is designed to stress test the pop/push operations of the recycled buffer
 * list.
 */

// Number of threads to use
#define THREAD_COUNT 8
// Number of iterations per thread
#define ITERATIONS 10000

static turbo_arena_t arena;
static volatile int stop_threads = 0;

// Thread function that repeatedly gets and returns buffers
static void buffer_churn_thread(void *arg) {
  int id = *(int *)arg;
  (void)id;

  for (int i = 0; i < ITERATIONS; i++) {
    // Allocate a buffer (triggering pop from recycle list if available)
    turbo_arena_buffer_t *buf = turbo_arena_get_pooled_buffer(&arena, 256);

    // If the list is corrupted, we might get NULL or invalid pointer causing
    // crash later
    if (buf) {
      // Touch the buffer to ensure it's valid memory (and catch access
      // violations) If buf is garbage (0xFF...), this writes to invalid
      // memory
      memset(buf->data, 0xAA, 16);

      // Simulate some work
      turbo_sleep_ms(0);

      // Return it (push to recycle list)
      turbo_arena_return_buffer(buf);
    }

    if (stop_threads)
      break;
  }
}

spec("Arena Multithread Tests") {

  it("should handle multithreaded recycling stress") {
    turbo_thread_t threads[THREAD_COUNT];
    int thread_ids[THREAD_COUNT];

    printf("Starting multi-threaded stress test with %d threads, %d iterations "
           "each...\n",
           THREAD_COUNT, ITERATIONS);

    // Pre-fill the pool with some buffers to encourage immediate contention on
    // pop
    for (int i = 0; i < 50; i++) {
      turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, 256);
      turbo_arena_return_buffer(buf);
    }

    // Start threads
    for (int i = 0; i < THREAD_COUNT; i++) {
      thread_ids[i] = i;
      int rc = turbo_thread_create(&threads[i], buffer_churn_thread, &thread_ids[i]);
      check_int_eq(rc, 0);
    }

    // Wait for threads
    for (int i = 0; i < THREAD_COUNT; i++) {
      turbo_thread_join(&threads[i]);
    }

    // Verify arena integrity (basic check)
    turbo_arena_stats_t stats;
    turbo_arena_get_stats(&arena, &stats);
    printf("Test complete. Total allocated: %zu, Regions: %zu\n", stats.total_allocated,
           stats.region_count);
  }
}
