#include "bucket_priority_queue_mpmc.h"
#include "tinytest.h"
#include "turbo_thread.h"
#include "turbo_atomic.h"

#include <stdbool.h>

#define TEST_ITEMS 10000
#define NUM_PRODUCERS 2
#define NUM_CONSUMERS 2

typedef struct {
  bucket_priority_queue_mpmc_t *queue;
  t_atomic_bool_t start;
  t_atomic_bool_t done;
  size_t items_to_process;
  size_t thread_id;
} producer_context_t;

typedef struct {
  bucket_priority_queue_mpmc_t *queue;
  t_atomic_bool_t start;
  t_atomic_bool_t done;
  t_atomic_uint32_t consumed_count;
  size_t target_count;
} consumer_context_t;

static void producer_thread(void *arg) {
  producer_context_t *ctx = (producer_context_t *)arg;

  while (!t_atomic_load_bool(&ctx->start)) {
    // Wait for start signal
  }

  for (size_t i = 0; i < ctx->items_to_process; ++i) {
    bucket_priority_mpmc_t priority = (bucket_priority_mpmc_t)(i % BUCKET_PRIORITY_MPMC_COUNT);
    size_t value = (ctx->thread_id << 32) | i;  // Encode thread_id in value

    // Retry on failure
    while (!bucket_priority_queue_mpmc_try_push(ctx->queue, priority, value)) {
      // Spin
    }
  }

  t_atomic_store_bool(&ctx->done, true);
}

static void consumer_thread(void *arg) {
  consumer_context_t *ctx = (consumer_context_t *)arg;

  while (!t_atomic_load_bool(&ctx->start)) {
    // Wait for start signal
  }

  while (t_atomic_load_uint32(&ctx->consumed_count) < ctx->target_count) {
    bucket_priority_mpmc_value_t value;
    if (bucket_priority_queue_mpmc_try_pop(ctx->queue, &value)) {
      t_atomic_fetch_add_uint32(&ctx->consumed_count, 1);
    }
  }

  t_atomic_store_bool(&ctx->done, true);
}

spec("Bucket Priority Queue MPMC") {
  it("starts empty after init") {
    bucket_priority_queue_mpmc_t queue;
    check(bucket_priority_queue_mpmc_init(&queue, 16, 4));
    bucket_priority_queue_mpmc_destroy(&queue);
  }

  it("supports single producer single consumer") {
    bucket_priority_queue_mpmc_t queue;
    check(bucket_priority_queue_mpmc_init(&queue, 16, 1));

    // Push items
    check(bucket_priority_queue_mpmc_try_push(&queue, BUCKET_PRIORITY_MPMC_LOW, 1));
    check(bucket_priority_queue_mpmc_try_push(&queue, BUCKET_PRIORITY_MPMC_CRITICAL, 4));
    check(bucket_priority_queue_mpmc_try_push(&queue, BUCKET_PRIORITY_MPMC_NORMAL, 2));
    check(bucket_priority_queue_mpmc_try_push(&queue, BUCKET_PRIORITY_MPMC_HIGH, 3));

    // Pop in priority order
    bucket_priority_mpmc_value_t value;
    check(bucket_priority_queue_mpmc_try_pop(&queue, &value));
    check_size_eq(value, 4);  // CRITICAL

    check(bucket_priority_queue_mpmc_try_pop(&queue, &value));
    check_size_eq(value, 3);  // HIGH

    check(bucket_priority_queue_mpmc_try_pop(&queue, &value));
    check_size_eq(value, 2);  // NORMAL

    check(bucket_priority_queue_mpmc_try_pop(&queue, &value));
    check_size_eq(value, 1);  // LOW

    bucket_priority_queue_mpmc_destroy(&queue);
  }

  it("handles blocking push") {
    bucket_priority_queue_mpmc_t queue;
    check(bucket_priority_queue_mpmc_init(&queue, 16, 1));

    bucket_priority_queue_mpmc_push_blocking(&queue, BUCKET_PRIORITY_MPMC_HIGH, 100);

    bucket_priority_mpmc_value_t value;
    check(bucket_priority_queue_mpmc_try_pop(&queue, &value));
    check_size_eq(value, 100);

    bucket_priority_queue_mpmc_destroy(&queue);
  }

  it("works with multiple producers and consumers") {
    bucket_priority_queue_mpmc_t queue;
    check(bucket_priority_queue_mpmc_init(&queue, 4096, NUM_CONSUMERS));

    // Setup producers
    producer_context_t producers[NUM_PRODUCERS];
    turbo_thread_t producer_threads[NUM_PRODUCERS];

    for (size_t i = 0; i < NUM_PRODUCERS; ++i) {
      producers[i].queue = &queue;
      producers[i].start = 0;
      producers[i].done = 0;
      producers[i].items_to_process = TEST_ITEMS / NUM_PRODUCERS;
      producers[i].thread_id = i;

      check(turbo_thread_create(&producer_threads[i], producer_thread, &producers[i]) == 0);
    }

    // Setup consumers
    consumer_context_t consumers[NUM_CONSUMERS];
    turbo_thread_t consumer_threads[NUM_CONSUMERS];

    for (size_t i = 0; i < NUM_CONSUMERS; ++i) {
      consumers[i].queue = &queue;
      consumers[i].start = 0;
      consumers[i].done = 0;
      consumers[i].consumed_count = 0;
      consumers[i].target_count = TEST_ITEMS / NUM_CONSUMERS;

      check(turbo_thread_create(&consumer_threads[i], consumer_thread, &consumers[i]) == 0);
    }

    // Start all threads
    for (size_t i = 0; i < NUM_PRODUCERS; ++i) {
      t_atomic_store_bool(&producers[i].start, true);
    }
    for (size_t i = 0; i < NUM_CONSUMERS; ++i) {
      t_atomic_store_bool(&consumers[i].start, true);
    }

    // Wait for completion
    for (size_t i = 0; i < NUM_PRODUCERS; ++i) {
      turbo_thread_join(&producer_threads[i]);
      check(t_atomic_load_bool(&producers[i].done));
    }

    for (size_t i = 0; i < NUM_CONSUMERS; ++i) {
      turbo_thread_join(&consumer_threads[i]);
      check(t_atomic_load_bool(&consumers[i].done));
    }

    // Verify total consumed
    uint32_t total_consumed = 0;
    for (size_t i = 0; i < NUM_CONSUMERS; ++i) {
      total_consumed += t_atomic_load_uint32(&consumers[i].consumed_count);
    }
    check_size_eq(total_consumed, TEST_ITEMS);

    bucket_priority_queue_mpmc_destroy(&queue);
  }
}
