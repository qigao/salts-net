#include "bucket_priority_queue.h"
#include "bucket_priority_queue_spsc.h"
#include "bucket_priority_queue_mpmc.h"
#include "tinytest.h"
#include "turbo_thread.h"
#include "turbo_atomic.h"

#include <stdint.h>
#include <stdio.h>

#define BENCH_ITERS (100000)
#define BENCH_ITERS_MT (50000)

static uint32_t g_rng_state = 1u;

static uint32_t bench_next_priority_bits(void) {
  g_rng_state = (g_rng_state * 1664525u) + 1013904223u;
  return g_rng_state & 0x3u;
}

/* ============================================================================
 * Single-threaded benchmarks
 * ============================================================================ */

spec("Priority Queue Comparison - Single Thread") {
  it("prints benchmark scope") {
    printf("\n=== Single-threaded Performance Comparison ===\n");
  }

  bench("Single-threaded: Push+Pop") {
    // Init outside benchmark loop to isolate queue operations
    bucket_priority_queue_t st_queue;
    bucket_priority_queue_spsc_t spsc_queue;
    bucket_priority_queue_mpmc_t mpmc_queue;

    bucket_priority_queue_init(&st_queue, 1024);
    bucket_priority_queue_spsc_init(&spsc_queue, 1024);
    bucket_priority_queue_mpmc_init(&mpmc_queue, 1024, 1);

    benchmark("ST (ring_buffer)", BENCH_ITERS) {
      bucket_priority_value_t out = 0;
      bucket_priority_queue_push(&st_queue, BUCKET_PRIORITY_HIGH, 42);
      bucket_priority_queue_pop(&st_queue, &out);
    }

    benchmark("SPSC (ring_buffer_spsc)", BENCH_ITERS) {
      bucket_priority_spsc_value_t out = 0;
      bucket_priority_queue_spsc_push(&spsc_queue, BUCKET_PRIORITY_SPSC_HIGH, 42);
      bucket_priority_queue_spsc_pop(&spsc_queue, &out);
    }

    benchmark("MPMC (disruptor)", BENCH_ITERS) {
      bucket_priority_mpmc_value_t out = 0;
      bucket_priority_queue_mpmc_try_push(&mpmc_queue, BUCKET_PRIORITY_MPMC_HIGH, 42);
      bucket_priority_queue_mpmc_try_pop(&mpmc_queue, &out);
    }

    bucket_priority_queue_mpmc_destroy(&mpmc_queue);
    bucket_priority_queue_spsc_destroy(&spsc_queue);
    bucket_priority_queue_destroy(&st_queue);
  }

  bench("Single-threaded: Push4+Pop4 (priority aware)") {
    // Init outside benchmark loop
    bucket_priority_queue_t st_queue;
    bucket_priority_queue_spsc_t spsc_queue;
    bucket_priority_queue_mpmc_t mpmc_queue;

    bucket_priority_queue_init(&st_queue, 1024);
    bucket_priority_queue_spsc_init(&spsc_queue, 1024);
    bucket_priority_queue_mpmc_init(&mpmc_queue, 1024, 1);

    benchmark("ST (ring_buffer)", BENCH_ITERS) {
      bucket_priority_value_t out = 0;
      bucket_priority_queue_push(&st_queue, BUCKET_PRIORITY_LOW, 1);
      bucket_priority_queue_push(&st_queue, BUCKET_PRIORITY_NORMAL, 2);
      bucket_priority_queue_push(&st_queue, BUCKET_PRIORITY_HIGH, 3);
      bucket_priority_queue_push(&st_queue, BUCKET_PRIORITY_CRITICAL, 4);
      bucket_priority_queue_pop(&st_queue, &out);
      bucket_priority_queue_pop(&st_queue, &out);
      bucket_priority_queue_pop(&st_queue, &out);
      bucket_priority_queue_pop(&st_queue, &out);
    }

    benchmark("SPSC (ring_buffer_spsc)", BENCH_ITERS) {
      bucket_priority_spsc_value_t out = 0;
      bucket_priority_queue_spsc_push(&spsc_queue, BUCKET_PRIORITY_SPSC_LOW, 1);
      bucket_priority_queue_spsc_push(&spsc_queue, BUCKET_PRIORITY_SPSC_NORMAL, 2);
      bucket_priority_queue_spsc_push(&spsc_queue, BUCKET_PRIORITY_SPSC_HIGH, 3);
      bucket_priority_queue_spsc_push(&spsc_queue, BUCKET_PRIORITY_SPSC_CRITICAL, 4);
      bucket_priority_queue_spsc_pop(&spsc_queue, &out);
      bucket_priority_queue_spsc_pop(&spsc_queue, &out);
      bucket_priority_queue_spsc_pop(&spsc_queue, &out);
      bucket_priority_queue_spsc_pop(&spsc_queue, &out);
    }

    benchmark("MPMC (disruptor)", BENCH_ITERS) {
      bucket_priority_mpmc_value_t out = 0;
      bucket_priority_queue_mpmc_try_push(&mpmc_queue, BUCKET_PRIORITY_MPMC_LOW, 1);
      bucket_priority_queue_mpmc_try_push(&mpmc_queue, BUCKET_PRIORITY_MPMC_NORMAL, 2);
      bucket_priority_queue_mpmc_try_push(&mpmc_queue, BUCKET_PRIORITY_MPMC_HIGH, 3);
      bucket_priority_queue_mpmc_try_push(&mpmc_queue, BUCKET_PRIORITY_MPMC_CRITICAL, 4);
      bucket_priority_queue_mpmc_try_pop(&mpmc_queue, &out);
      bucket_priority_queue_mpmc_try_pop(&mpmc_queue, &out);
      bucket_priority_queue_mpmc_try_pop(&mpmc_queue, &out);
      bucket_priority_queue_mpmc_try_pop(&mpmc_queue, &out);
    }

    bucket_priority_queue_mpmc_destroy(&mpmc_queue);
    bucket_priority_queue_spsc_destroy(&spsc_queue);
    bucket_priority_queue_destroy(&st_queue);
  }

  bench("Single-threaded: Random priority distribution") {
    // Init outside benchmark loop to isolate queue operations
    bucket_priority_queue_t st_queue;
    bucket_priority_queue_spsc_t spsc_queue;
    bucket_priority_queue_mpmc_t mpmc_queue;

    bucket_priority_queue_init(&st_queue, 1024);
    bucket_priority_queue_spsc_init(&spsc_queue, 1024);
    bucket_priority_queue_mpmc_init(&mpmc_queue, 1024, 1);

    benchmark("ST (ring_buffer)", BENCH_ITERS) {
      bucket_priority_value_t out = 0;

      g_rng_state = 1u;
      for (size_t i = 0; i < 100; ++i) {
        bucket_priority_queue_push(&st_queue, (bucket_priority_t)bench_next_priority_bits(), i);
      }
      for (size_t i = 0; i < 100; ++i) {
        bucket_priority_queue_pop(&st_queue, &out);
      }
    }

    benchmark("SPSC (ring_buffer_spsc)", BENCH_ITERS) {
      bucket_priority_spsc_value_t out = 0;

      g_rng_state = 1u;
      for (size_t i = 0; i < 100; ++i) {
        bucket_priority_queue_spsc_push(&spsc_queue, (bucket_priority_spsc_t)bench_next_priority_bits(), i);
      }
      for (size_t i = 0; i < 100; ++i) {
        bucket_priority_queue_spsc_pop(&spsc_queue, &out);
      }
    }

    benchmark("MPMC (disruptor)", BENCH_ITERS) {
      bucket_priority_mpmc_value_t out = 0;

      g_rng_state = 1u;
      for (size_t i = 0; i < 100; ++i) {
        bucket_priority_queue_mpmc_try_push(&mpmc_queue, (bucket_priority_mpmc_t)bench_next_priority_bits(), i);
      }
      for (size_t i = 0; i < 100; ++i) {
        bucket_priority_queue_mpmc_try_pop(&mpmc_queue, &out);
      }
    }

    bucket_priority_queue_mpmc_destroy(&mpmc_queue);
    bucket_priority_queue_spsc_destroy(&spsc_queue);
    bucket_priority_queue_destroy(&st_queue);
  }
}

/* ============================================================================
 * Multi-threaded benchmarks (SPSC vs MPMC)
 * ============================================================================ */

typedef struct {
  void *queue;
  turbo_atomic_bool_t start;
  turbo_atomic_uint32_t produced;
  turbo_atomic_uint32_t consumed;
  size_t target;
} mt_bench_context_t;

static void spsc_producer(void *arg) {
  mt_bench_context_t *ctx = (mt_bench_context_t *)arg;
  bucket_priority_queue_spsc_t *queue = (bucket_priority_queue_spsc_t *)ctx->queue;

  while (!turbo_atomic_load_bool(&ctx->start)) {}

  for (size_t i = 0; i < ctx->target; ++i) {
    bucket_priority_spsc_t priority = (bucket_priority_spsc_t)(i % BUCKET_PRIORITY_SPSC_COUNT);
    while (!bucket_priority_queue_spsc_push(queue, priority, i)) {}
    turbo_atomic_fetch_add_uint32(&ctx->produced, 1);
  }
}

static void spsc_consumer(void *arg) {
  mt_bench_context_t *ctx = (mt_bench_context_t *)arg;
  bucket_priority_queue_spsc_t *queue = (bucket_priority_queue_spsc_t *)ctx->queue;

  while (!turbo_atomic_load_bool(&ctx->start)) {}

  bucket_priority_spsc_value_t value;
  while (turbo_atomic_load_uint32(&ctx->consumed) < ctx->target) {
    if (bucket_priority_queue_spsc_pop(queue, &value)) {
      turbo_atomic_fetch_add_uint32(&ctx->consumed, 1);
    }
  }
}

static void mpmc_producer(void *arg) {
  mt_bench_context_t *ctx = (mt_bench_context_t *)arg;
  bucket_priority_queue_mpmc_t *queue = (bucket_priority_queue_mpmc_t *)ctx->queue;

  while (!turbo_atomic_load_bool(&ctx->start)) {}

  for (size_t i = 0; i < ctx->target; ++i) {
    bucket_priority_mpmc_t priority = (bucket_priority_mpmc_t)(i % BUCKET_PRIORITY_MPMC_COUNT);
    while (!bucket_priority_queue_mpmc_try_push(queue, priority, i)) {}
    turbo_atomic_fetch_add_uint32(&ctx->produced, 1);
  }
}

static void mpmc_consumer(void *arg) {
  mt_bench_context_t *ctx = (mt_bench_context_t *)arg;
  bucket_priority_queue_mpmc_t *queue = (bucket_priority_queue_mpmc_t *)ctx->queue;

  while (!turbo_atomic_load_bool(&ctx->start)) {}

  bucket_priority_mpmc_value_t value;
  while (turbo_atomic_load_uint32(&ctx->consumed) < ctx->target) {
    if (bucket_priority_queue_mpmc_try_pop(queue, &value)) {
      turbo_atomic_fetch_add_uint32(&ctx->consumed, 1);
    }
  }
}

spec("Priority Queue Comparison - Multi-threaded") {
  it("prints benchmark scope") {
    printf("\n=== Multi-threaded Performance Comparison ===\n");
  }

  bench("SPSC: 1 Producer + 1 Consumer") {
    benchmark("SPSC throughput", BENCH_ITERS_MT) {
      bucket_priority_queue_spsc_t queue;
      bucket_priority_queue_spsc_init(&queue, 4096);

      mt_bench_context_t ctx = {
        .queue = &queue,
        .start = 0,
        .produced = 0,
        .consumed = 0,
        .target = 10000
      };

      turbo_thread_t producer, consumer;
      turbo_thread_create(&producer, spsc_producer, &ctx);
      turbo_thread_create(&consumer, spsc_consumer, &ctx);

      turbo_atomic_store_bool(&ctx.start, true);

      turbo_thread_join(&producer);
      turbo_thread_join(&consumer);

      bucket_priority_queue_spsc_destroy(&queue);
    }
  }

  bench("MPMC: 1 Producer + 1 Consumer") {
    benchmark("MPMC throughput", BENCH_ITERS_MT) {
      bucket_priority_queue_mpmc_t queue;
      bucket_priority_queue_mpmc_init(&queue, 4096, 1);

      mt_bench_context_t ctx = {
        .queue = &queue,
        .start = 0,
        .produced = 0,
        .consumed = 0,
        .target = 10000
      };

      turbo_thread_t producer, consumer;
      turbo_thread_create(&producer, mpmc_producer, &ctx);
      turbo_thread_create(&consumer, mpmc_consumer, &ctx);

      turbo_atomic_store_bool(&ctx.start, true);

      turbo_thread_join(&producer);
      turbo_thread_join(&consumer);

      bucket_priority_queue_mpmc_destroy(&queue);
    }
  }
}
