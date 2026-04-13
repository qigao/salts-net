#include "bucket_priority_queue.h"
#include "tinytest.h"

#include <stdint.h>
#include <stdio.h>

#define BENCH_ITERS (200000)
#define BENCH_ITERS_SMALL (50000)
#define BENCH_ITERS_MIXED (20000)
#define BENCH_ITERS_GROWTH (2000)
#define BENCH_ITERS_DIST (4000)
#define BATCH_SIZE 64
#define MIXED_OPS_PER_ITER 1024
#define GROWTH_TARGET_DEPTH 4096
#define DIST_FIXED_DEPTH 4096
#define DIST_STEPS_PER_ITER 1024
#define DIST_STEADY_STEPS_PER_ITER 1024

static bucket_priority_queue_t g_queue;
static bucket_priority_value_t g_batch_out[BATCH_SIZE];
static uint32_t g_rng_state = 1u;

static bucket_priority_t bench_next_priority(void) {
  g_rng_state = (g_rng_state * 1664525u) + 1013904223u;
  return (bucket_priority_t)(g_rng_state & 0x3u);
}

typedef enum {
  DIST_UNIFORM = 0,
  DIST_HIGH_HEAVY = 1,
  DIST_CRITICAL_BURST = 2
} bench_dist_mode_t;

static bucket_priority_t bench_dist_priority(bench_dist_mode_t mode, size_t index) {
  switch (mode) {
  case DIST_UNIFORM:
    return (bucket_priority_t)(index & 0x3u);
  case DIST_HIGH_HEAVY:
    if ((index & 0x1Fu) == 0) {
      return BUCKET_PRIORITY_CRITICAL;
    }
    if ((index & 0x7u) == 0) {
      return BUCKET_PRIORITY_NORMAL;
    }
    return BUCKET_PRIORITY_HIGH;
  case DIST_CRITICAL_BURST:
    if ((index & 0x3Fu) < 8u) {
      return BUCKET_PRIORITY_CRITICAL;
    }
    if ((index & 0x3u) == 0u) {
      return BUCKET_PRIORITY_HIGH;
    }
    return BUCKET_PRIORITY_LOW;
  default:
    return BUCKET_PRIORITY_NORMAL;
  }
}

static void bench_fill_depth(bucket_priority_queue_t *queue, size_t depth, bench_dist_mode_t mode) {
  size_t i = 0;
  for (i = 0; i < depth; ++i) {
    (void)bucket_priority_queue_push(queue, bench_dist_priority(mode, i), i);
  }
}

spec("Bucket Priority Queue Benchmarks") {
  before_each() {
    /* Pre-reserve to isolate queue logic instead of allocator overhead. */
    check(bucket_priority_queue_init(&g_queue, 2048));
    g_rng_state = 1u;
  }

  after_each() { bucket_priority_queue_destroy(&g_queue); }

  it("prints benchmark scope") {
    printf("\n=== bucket_priority_queue (C11 + FIFO + ring_buffer buckets) ===\n");
  }

  bench("Core Operations") {

      benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
    benchmark("Push+Pop (same priority)", BENCH_ITERS, 1) {
      bucket_priority_value_t out = 0;
      (void)bucket_priority_queue_push(&g_queue, BUCKET_PRIORITY_HIGH, 42);
      (void)bucket_priority_queue_pop(&g_queue, &out);
    }

    benchmark("Push4+Pop4 (priority aware)", BENCH_ITERS, 1) {
      bucket_priority_value_t out = 0;
      (void)bucket_priority_queue_push(&g_queue, BUCKET_PRIORITY_LOW, 1);
      (void)bucket_priority_queue_push(&g_queue, BUCKET_PRIORITY_NORMAL, 2);
      (void)bucket_priority_queue_push(&g_queue, BUCKET_PRIORITY_HIGH, 3);
      (void)bucket_priority_queue_push(&g_queue, BUCKET_PRIORITY_CRITICAL, 4);
      (void)bucket_priority_queue_pop(&g_queue, &out);
      (void)bucket_priority_queue_pop(&g_queue, &out);
      (void)bucket_priority_queue_pop(&g_queue, &out);
      (void)bucket_priority_queue_pop(&g_queue, &out);
    }

    benchmark("Push64+PopBatch64", BENCH_ITERS_SMALL, 1) {
      size_t i = 0;
      for (i = 0; i < BATCH_SIZE; ++i) {
        (void)bucket_priority_queue_push(&g_queue, (bucket_priority_t)(i & 0x3u), i);
      }
      (void)bucket_priority_queue_pop_batch(&g_queue, BATCH_SIZE, g_batch_out);
    }
  }

  bench("Mixed Workload") {

      benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
    benchmark("70% push / 30% pop (1024 ops)", BENCH_ITERS_MIXED, 1) {
      size_t i = 0;
      bucket_priority_value_t out = 0;

      for (i = 0; i < MIXED_OPS_PER_ITER; ++i) {
        if ((i & 0xF) < 11) {
          (void)bucket_priority_queue_push(&g_queue, bench_next_priority(), i);
        } else {
          (void)bucket_priority_queue_pop(&g_queue, &out);
        }
      }

      /* Keep allocated capacity across iterations but reset state. */
      bucket_priority_queue_clear(&g_queue);
    }
  }

  bench("No-Reserve Growth") {

      benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
    benchmark("Cold growth 0->4096 + drain", BENCH_ITERS_GROWTH, 1) {
      bucket_priority_queue_t cold_queue = {0};
      bucket_priority_value_t out = 0;

      if (bucket_priority_queue_init(&cold_queue, 0)) {
        bench_fill_depth(&cold_queue, GROWTH_TARGET_DEPTH, DIST_UNIFORM);
        while (bucket_priority_queue_pop(&cold_queue, &out)) {
        }
        bucket_priority_queue_destroy(&cold_queue);
      }
    }
  }

  bench("Priority Distribution Matrix (Fixed Depth=4096)") {

      benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
    benchmark("Uniform distribution", BENCH_ITERS_DIST, 1) {
      size_t i = 0;
      bucket_priority_value_t out = 0;

      bucket_priority_queue_clear(&g_queue);
      bench_fill_depth(&g_queue, DIST_FIXED_DEPTH, DIST_UNIFORM);
      for (i = 0; i < DIST_STEPS_PER_ITER; ++i) {
        (void)bucket_priority_queue_pop(&g_queue, &out);
        (void)bucket_priority_queue_push(&g_queue, bench_dist_priority(DIST_UNIFORM, i), i);
      }
    }

    benchmark("High-heavy (90% HIGH-ish)", BENCH_ITERS_DIST, 1) {
      size_t i = 0;
      bucket_priority_value_t out = 0;

      bucket_priority_queue_clear(&g_queue);
      bench_fill_depth(&g_queue, DIST_FIXED_DEPTH, DIST_HIGH_HEAVY);
      for (i = 0; i < DIST_STEPS_PER_ITER; ++i) {
        (void)bucket_priority_queue_pop(&g_queue, &out);
        (void)bucket_priority_queue_push(&g_queue, bench_dist_priority(DIST_HIGH_HEAVY, i), i);
      }
    }

    benchmark("Critical burst pattern", BENCH_ITERS_DIST, 1) {
      size_t i = 0;
      bucket_priority_value_t out = 0;

      bucket_priority_queue_clear(&g_queue);
      bench_fill_depth(&g_queue, DIST_FIXED_DEPTH, DIST_CRITICAL_BURST);
      for (i = 0; i < DIST_STEPS_PER_ITER; ++i) {
        (void)bucket_priority_queue_pop(&g_queue, &out);
        (void)bucket_priority_queue_push(&g_queue, bench_dist_priority(DIST_CRITICAL_BURST, i), i);
      }
    }
  }

  bench("Priority Distribution Steady-State (No Refill)") {

      benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
    bucket_priority_queue_t q_uniform = {0};
    bucket_priority_queue_t q_high_heavy = {0};
    bucket_priority_queue_t q_critical_burst = {0};
    size_t seq_uniform = 0;
    size_t seq_high_heavy = 0;
    size_t seq_critical_burst = 0;

    check(bucket_priority_queue_init(&q_uniform, 2048));
    check(bucket_priority_queue_init(&q_high_heavy, 2048));
    check(bucket_priority_queue_init(&q_critical_burst, 2048));

    bench_fill_depth(&q_uniform, DIST_FIXED_DEPTH, DIST_UNIFORM);
    bench_fill_depth(&q_high_heavy, DIST_FIXED_DEPTH, DIST_HIGH_HEAVY);
    bench_fill_depth(&q_critical_burst, DIST_FIXED_DEPTH, DIST_CRITICAL_BURST);

    benchmark("Uniform steady-state", BENCH_ITERS_DIST, 1) {
      size_t i = 0;
      bucket_priority_value_t out = 0;
      for (i = 0; i < DIST_STEADY_STEPS_PER_ITER; ++i) {
        (void)bucket_priority_queue_pop(&q_uniform, &out);
        (void)bucket_priority_queue_push(&q_uniform, bench_dist_priority(DIST_UNIFORM, seq_uniform),
                                         seq_uniform);
        seq_uniform++;
      }
    }

    benchmark("High-heavy steady-state", BENCH_ITERS_DIST, 1) {
      size_t i = 0;
      bucket_priority_value_t out = 0;
      for (i = 0; i < DIST_STEADY_STEPS_PER_ITER; ++i) {
        (void)bucket_priority_queue_pop(&q_high_heavy, &out);
        (void)bucket_priority_queue_push(&q_high_heavy,
                                         bench_dist_priority(DIST_HIGH_HEAVY, seq_high_heavy),
                                         seq_high_heavy);
        seq_high_heavy++;
      }
    }

    benchmark("Critical burst steady-state", BENCH_ITERS_DIST, 1) {
      size_t i = 0;
      bucket_priority_value_t out = 0;
      for (i = 0; i < DIST_STEADY_STEPS_PER_ITER; ++i) {
        (void)bucket_priority_queue_pop(&q_critical_burst, &out);
        (void)bucket_priority_queue_push(
            &q_critical_burst, bench_dist_priority(DIST_CRITICAL_BURST, seq_critical_burst),
            seq_critical_burst);
        seq_critical_burst++;
      }
    }

    bucket_priority_queue_destroy(&q_uniform);
    bucket_priority_queue_destroy(&q_high_heavy);
    bucket_priority_queue_destroy(&q_critical_burst);
  }
}
