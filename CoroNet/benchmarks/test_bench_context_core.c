/**
 * @file test_bench_context_core.c
 * @brief Focused CoroNet context-queue and lazy-task lifecycle benchmarks.
 *
 * The SPSC and Disruptor cases are intentionally busy-polling queue upper
 * bounds. They are not drop-in replacements for coro_post(), which also wakes
 * a sleeping event loop and reports a full queue as TURBO_ENOMEM.
 */

#include "CoroNet.h"
#include "disruptor.h"
#include "ring_buffer_spsc.h"
#include "tinytest.h"
#include "turbo_thread.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  BENCH_LAZY_TASK_SMALL = 64,
  BENCH_LAZY_TASK_MEDIUM = 512,
  BENCH_LAZY_TASK_LARGE = 4096,
  BENCH_POST_BATCH = 8192,
  BENCH_POST_PRODUCERS = 4,
  BENCH_POSTS_PER_PRODUCER = BENCH_POST_BATCH / BENCH_POST_PRODUCERS,
  BENCH_SPSC_STORAGE_SIZE = 65536,
  BENCH_SPSC_LANE_STORAGE_SIZE = 16384,
  BENCH_WAKE_SAMPLES = 512,
  BENCH_WAKE_IDLE_MS = 1,
  BENCH_WAIT_TIMEOUT_MS = 10000
};

static volatile int g_context_bench_sink;

static void bench_noop_task(coro_t *co, void *arg) {
  (void)co;
  (void)arg;
}

static int run_lazy_task_batch(coro_context_t *ctx, size_t task_count) {
  int rc = TURBO_OK;
  if (task_count > SIZE_MAX / sizeof(coro_task_t *)) return TURBO_ENOMEM;

  coro_task_t **tasks = (coro_task_t **)malloc(task_count * sizeof(*tasks));
  if (!tasks) return TURBO_ENOMEM;

  size_t created = 0;
  for (; created < task_count; ++created) {
    tasks[created] = coro_task_create(ctx, bench_noop_task, NULL);
    if (!tasks[created]) break;
  }

  if (created != task_count) rc = TURBO_ENOMEM;
  for (size_t i = 0; i < created; ++i) {
    if (coro_task_cancel(tasks[i]) != TURBO_OK) rc = TURBO_EINVAL;
  }
  for (size_t i = 0; i < created; ++i) {
    coro_task_destroy(tasks[i]);
  }

  free(tasks);
  return rc;
}

static int wait_for_atomic_value(atomic_int *value, int expected) {
  const uint64_t deadline = turbo_monotonic_ms() + BENCH_WAIT_TIMEOUT_MS;
  while (atomic_load_explicit(value, memory_order_acquire) != expected) {
    if (turbo_monotonic_ms() >= deadline) return TURBO_ETIMEDOUT;
    turbo_thread_yield();
  }
  return TURBO_OK;
}

static void context_runner(void *arg) {
  (void)coro_context_run((coro_context_t *)arg, TURBO_RUN_DEFAULT);
}

static void count_post(void *arg1, void *arg2) {
  (void)arg2;
  atomic_fetch_add_explicit((atomic_int *)arg1, 1, memory_order_release);
}

static int post_with_backpressure(coro_context_t *ctx, atomic_int *counter) {
  int rc;
  do {
    rc = coro_post(ctx, count_post, (void *)counter, NULL);
    if (rc == TURBO_ENOMEM) turbo_thread_yield();
  } while (rc == TURBO_ENOMEM);
  return rc;
}

typedef struct {
  coro_context_t *ctx;
  atomic_int *callback_count;
  atomic_int *epoch;
  atomic_int *producer_done;
  atomic_int *error;
  atomic_int *stop;
} post_producer_state_t;

static void post_producer(void *arg) {
  post_producer_state_t *state = (post_producer_state_t *)arg;
  int observed_epoch = 0;

  for (;;) {
    const int epoch = atomic_load_explicit(state->epoch, memory_order_acquire);
    if (epoch == observed_epoch) {
      turbo_thread_yield();
      continue;
    }
    observed_epoch = epoch;

    if (atomic_load_explicit(state->stop, memory_order_acquire)) return;

    for (int i = 0; i < BENCH_POSTS_PER_PRODUCER; ++i) {
      const int rc = post_with_backpressure(state->ctx, state->callback_count);
      if (rc != TURBO_OK) {
        atomic_store_explicit(state->error, rc, memory_order_release);
        break;
      }
    }
    atomic_fetch_add_explicit(state->producer_done, 1, memory_order_release);
  }
}

typedef struct {
  ring_spsc_t *ring;
  atomic_int *consumed;
  atomic_int *stop;
} spsc_consumer_state_t;

static void spsc_consumer(void *arg) {
  spsc_consumer_state_t *state = (spsc_consumer_state_t *)arg;

  for (;;) {
    size_t available = 0;
    uint8_t *read_ptr = ring_spsc_read_acquire(state->ring, &available);
    if (read_ptr && available > 0) {
      ring_spsc_read_release(state->ring, available);
      atomic_fetch_add_explicit(state->consumed, (int)available, memory_order_release);
      continue;
    }
    if (atomic_load_explicit(state->stop, memory_order_acquire) &&
        ring_spsc_read_available(state->ring) == 0) {
      return;
    }
    turbo_thread_yield();
  }
}

typedef struct {
  ring_spsc_t *rings;
  atomic_int *consumed;
  atomic_int *error;
  atomic_int *stop;
} spsc_fan_in_consumer_state_t;

static void spsc_fan_in_consumer(void *arg) {
  spsc_fan_in_consumer_state_t *state = (spsc_fan_in_consumer_state_t *)arg;

  for (;;) {
    int progressed = 0;
    int pending = 0;

    for (int i = 0; i < BENCH_POST_PRODUCERS; ++i) {
      size_t available = 0;
      uint8_t *read_ptr = ring_spsc_read_acquire(&state->rings[i], &available);
      (void)read_ptr;

      if (available > 0) {
        const size_t item_count = available / sizeof(uint64_t);
        if ((available % sizeof(uint64_t)) != 0 || item_count > (size_t)INT_MAX) {
          atomic_store_explicit(state->error, TURBO_EINVAL, memory_order_release);
          return;
        }
        ring_spsc_read_release(&state->rings[i], item_count * sizeof(uint64_t));
        atomic_fetch_add_explicit(state->consumed, (int)item_count, memory_order_release);
        progressed = 1;
      }

      if (ring_spsc_read_available(&state->rings[i]) != 0) pending = 1;
    }

    if (atomic_load_explicit(state->stop, memory_order_acquire) && !pending && !progressed) {
      return;
    }
    if (!progressed) turbo_thread_yield();
  }
}

typedef struct {
  ring_spsc_t *ring;
  atomic_int *epoch;
  atomic_int *producer_done;
  atomic_int *stop;
} spsc_lane_producer_state_t;

static void spsc_lane_producer(void *arg) {
  spsc_lane_producer_state_t *state = (spsc_lane_producer_state_t *)arg;
  int observed_epoch = 0;

  for (;;) {
    const int epoch = atomic_load_explicit(state->epoch, memory_order_acquire);
    if (epoch == observed_epoch) {
      turbo_thread_yield();
      continue;
    }
    observed_epoch = epoch;

    if (atomic_load_explicit(state->stop, memory_order_acquire)) return;

    for (int i = 0; i < BENCH_POSTS_PER_PRODUCER; ++i) {
      const uint64_t value = (uint64_t)i;
      uint8_t *write_ptr;
      do {
        write_ptr = ring_spsc_write_acquire(state->ring, sizeof(value));
        if (!write_ptr) turbo_thread_yield();
      } while (!write_ptr);
      memcpy(write_ptr, &value, sizeof(value));
      ring_spsc_write_release(state->ring, sizeof(value));
    }
    atomic_fetch_add_explicit(state->producer_done, 1, memory_order_release);
  }
}

typedef struct {
  disruptor_t *queue;
  disruptor_consumer_t consumer;
  uint64_t next_sequence;
  atomic_int *consumed;
  atomic_int *stop;
} disruptor_consumer_state_t;

static void disruptor_consumer(void *arg) {
  disruptor_consumer_state_t *state = (disruptor_consumer_state_t *)arg;

  for (;;) {
    disruptor_cursor_t cursor = {state->next_sequence};
    if (disruptor_consumer_wait_for_nonblocking(state->queue, &cursor)) {
      const uint64_t count = cursor.sequence - state->next_sequence + 1U;
      disruptor_consumer_release_entry(state->queue, &state->consumer, &cursor);
      state->next_sequence = cursor.sequence + 1U;
      atomic_fetch_add_explicit(state->consumed, (int)count, memory_order_release);
      continue;
    }
    if (atomic_load_explicit(state->stop, memory_order_acquire)) return;
    turbo_thread_yield();
  }
}

static int disruptor_publish_item(disruptor_t *queue, uint64_t value) {
  disruptor_cursor_t cursor = {0};
  uint64_t *entry;

  disruptor_publisher_next_entry_blocking(queue, &cursor);
  if (cursor.sequence == 0U) return TURBO_EINVAL;
  entry = (uint64_t *)disruptor_acquire_entry(queue, &cursor);
  if (!entry) return TURBO_EINVAL;
  *entry = value;
  return disruptor_publisher_publish(queue, &cursor) ? TURBO_OK : TURBO_EINVAL;
}

typedef struct {
  disruptor_t *queue;
  atomic_int *epoch;
  atomic_int *producer_done;
  atomic_int *error;
  atomic_int *stop;
} disruptor_producer_state_t;

static void disruptor_producer(void *arg) {
  disruptor_producer_state_t *state = (disruptor_producer_state_t *)arg;
  int observed_epoch = 0;

  for (;;) {
    const int epoch = atomic_load_explicit(state->epoch, memory_order_acquire);
    if (epoch == observed_epoch) {
      turbo_thread_yield();
      continue;
    }
    observed_epoch = epoch;

    if (atomic_load_explicit(state->stop, memory_order_acquire)) return;

    for (int i = 0; i < BENCH_POSTS_PER_PRODUCER; ++i) {
      const int rc = disruptor_publish_item(state->queue, (uint64_t)i);
      if (rc != TURBO_OK) {
        atomic_store_explicit(state->error, rc, memory_order_release);
        break;
      }
    }
    atomic_fetch_add_explicit(state->producer_done, 1, memory_order_release);
  }
}

typedef struct {
  atomic_uint_fast64_t started_ns;
  atomic_int completed;
  uint64_t samples_ns[BENCH_WAKE_SAMPLES];
} wake_latency_state_t;

static void record_wake_latency(void *arg1, void *arg2) {
  wake_latency_state_t *state = (wake_latency_state_t *)arg1;
  const int index = atomic_load_explicit(&state->completed, memory_order_relaxed);
  const uint64_t started_ns = atomic_load_explicit(&state->started_ns, memory_order_acquire);
  (void)arg2;

  state->samples_ns[index] = turbo_hrtime() - started_ns;
  atomic_store_explicit(&state->completed, index + 1, memory_order_release);
}

static int compare_u64(const void *lhs, const void *rhs) {
  const uint64_t left = *(const uint64_t *)lhs;
  const uint64_t right = *(const uint64_t *)rhs;
  return (left > right) - (left < right);
}

static uint64_t percentile_nearest_rank(const uint64_t *sorted, size_t count, size_t percentile) {
  const size_t rank = ((percentile * count) + 99U) / 100U;
  return sorted[rank - 1U];
}

spec("coronet context core benchmark") {
  bench("lazy task registry") {
    coro_context_t *ctx = coro_context_create(NULL);
    int rc = TURBO_OK;
    check_not_null(ctx);

    benchmark_titles("benchmark", "tasks", "tasks", "avg/task(us)", NULL, "min_batch(us)",
                     "max_batch(us)", "tasks/s", NULL, NULL);

    benchmark("create_cancel_destroy_64", 100, BENCH_LAZY_TASK_SMALL) {
      rc = run_lazy_task_batch(ctx, BENCH_LAZY_TASK_SMALL);
      g_context_bench_sink = rc;
    }
    check_int_eq(rc, TURBO_OK);

    benchmark("create_cancel_destroy_512", 30, BENCH_LAZY_TASK_MEDIUM) {
      rc = run_lazy_task_batch(ctx, BENCH_LAZY_TASK_MEDIUM);
      g_context_bench_sink = rc;
    }
    check_int_eq(rc, TURBO_OK);

    benchmark("create_cancel_destroy_4096", 10, BENCH_LAZY_TASK_LARGE) {
      rc = run_lazy_task_batch(ctx, BENCH_LAZY_TASK_LARGE);
      g_context_bench_sink = rc;
    }
    check_int_eq(rc, TURBO_OK);

    coro_context_destroy(ctx);
  }

  bench("context post handoff") {
    coro_context_t *ctx = coro_context_create(NULL);
    turbo_thread_t context_thread;
    atomic_int callback_count;
    int rc = TURBO_OK;
    check_not_null(ctx);
    atomic_init(&callback_count, 0);
    coro_context_set_persistent(ctx, 1);
    check_int_eq(turbo_thread_create(&context_thread, context_runner, ctx), TURBO_OK);

    benchmark_titles("benchmark", "posts", "posts", "avg/post(us)", NULL, "min_batch(us)",
                     "max_batch(us)", "posts/s", NULL, NULL);

    benchmark("coro_post_1p_8192", 20, BENCH_POST_BATCH) {
      atomic_store_explicit(&callback_count, 0, memory_order_release);
      for (int i = 0; i < BENCH_POST_BATCH; ++i) {
        rc = post_with_backpressure(ctx, &callback_count);
        if (rc != TURBO_OK) break;
      }
      if (rc == TURBO_OK) rc = wait_for_atomic_value(&callback_count, BENCH_POST_BATCH);
      g_context_bench_sink = atomic_load_explicit(&callback_count, memory_order_relaxed);
    }
    check_int_eq(rc, TURBO_OK);
    check_int_eq(atomic_load_explicit(&callback_count, memory_order_acquire), BENCH_POST_BATCH);

    coro_context_set_persistent(ctx, 0);
    coro_context_stop(ctx);
    check_int_eq(turbo_thread_join(&context_thread), TURBO_OK);
    turbo_thread_destroy(&context_thread);
    coro_context_destroy(ctx);
  }

  bench("context post contention") {
    coro_context_t *ctx = coro_context_create(NULL);
    turbo_thread_t context_thread;
    turbo_thread_t producers[BENCH_POST_PRODUCERS];
    post_producer_state_t producer_states[BENCH_POST_PRODUCERS];
    atomic_int callback_count;
    atomic_int epoch;
    atomic_int producer_done;
    atomic_int error;
    atomic_int stop;
    int rc = TURBO_OK;
    check_not_null(ctx);

    atomic_init(&callback_count, 0);
    atomic_init(&epoch, 0);
    atomic_init(&producer_done, 0);
    atomic_init(&error, TURBO_OK);
    atomic_init(&stop, 0);
    coro_context_set_persistent(ctx, 1);
    check_int_eq(turbo_thread_create(&context_thread, context_runner, ctx), TURBO_OK);

    for (int i = 0; i < BENCH_POST_PRODUCERS; ++i) {
      producer_states[i] =
          (post_producer_state_t){ctx, &callback_count, &epoch, &producer_done, &error, &stop};
      check_int_eq(turbo_thread_create(&producers[i], post_producer, &producer_states[i]),
                   TURBO_OK);
    }

    benchmark_titles("benchmark", "posts", "posts", "avg/post(us)", NULL, "min_batch(us)",
                     "max_batch(us)", "posts/s", NULL, NULL);

    benchmark("coro_post_4p_8192", 20, BENCH_POST_BATCH) {
      atomic_store_explicit(&callback_count, 0, memory_order_release);
      atomic_store_explicit(&producer_done, 0, memory_order_release);
      atomic_store_explicit(&error, TURBO_OK, memory_order_release);
      atomic_fetch_add_explicit(&epoch, 1, memory_order_release);

      rc = wait_for_atomic_value(&producer_done, BENCH_POST_PRODUCERS);
      if (rc == TURBO_OK) rc = atomic_load_explicit(&error, memory_order_acquire);
      if (rc == TURBO_OK) rc = wait_for_atomic_value(&callback_count, BENCH_POST_BATCH);
      g_context_bench_sink = atomic_load_explicit(&callback_count, memory_order_relaxed);
    }
    check_int_eq(rc, TURBO_OK);
    check_int_eq(atomic_load_explicit(&callback_count, memory_order_acquire), BENCH_POST_BATCH);

    atomic_store_explicit(&stop, 1, memory_order_release);
    atomic_fetch_add_explicit(&epoch, 1, memory_order_release);
    for (int i = 0; i < BENCH_POST_PRODUCERS; ++i) {
      check_int_eq(turbo_thread_join(&producers[i]), TURBO_OK);
      turbo_thread_destroy(&producers[i]);
    }

    coro_context_set_persistent(ctx, 0);
    coro_context_stop(ctx);
    check_int_eq(turbo_thread_join(&context_thread), TURBO_OK);
    turbo_thread_destroy(&context_thread);
    coro_context_destroy(ctx);
  }

  bench("TurboUtils SPSC upper bound") {
    uint8_t storage[BENCH_SPSC_STORAGE_SIZE];
    ring_spsc_t ring;
    turbo_thread_t consumer_thread;
    atomic_int consumed;
    atomic_int stop;
    spsc_consumer_state_t consumer_state;
    int rc = TURBO_OK;
    check_true(ring_spsc_init(&ring, storage, sizeof(storage)));
    atomic_init(&consumed, 0);
    atomic_init(&stop, 0);
    consumer_state = (spsc_consumer_state_t){&ring, &consumed, &stop};
    check_int_eq(turbo_thread_create(&consumer_thread, spsc_consumer, &consumer_state), TURBO_OK);

    benchmark_titles("benchmark", "items", "items", "avg/item(us)", NULL, "min_batch(us)",
                     "max_batch(us)", "items/s", NULL, NULL);

    benchmark("ring_spsc_1p_8192_busy_poll", 20, BENCH_POST_BATCH) {
      atomic_store_explicit(&consumed, 0, memory_order_release);
      for (int i = 0; i < BENCH_POST_BATCH; ++i) {
        uint8_t *write_ptr;
        do {
          write_ptr = ring_spsc_write_acquire(&ring, 1);
          if (!write_ptr) turbo_thread_yield();
        } while (!write_ptr);
        *write_ptr = (uint8_t)i;
        ring_spsc_write_release(&ring, 1);
      }
      rc = wait_for_atomic_value(&consumed, BENCH_POST_BATCH);
      g_context_bench_sink = atomic_load_explicit(&consumed, memory_order_relaxed);
    }
    check_int_eq(rc, TURBO_OK);
    check_int_eq(atomic_load_explicit(&consumed, memory_order_acquire), BENCH_POST_BATCH);

    atomic_store_explicit(&stop, 1, memory_order_release);
    check_int_eq(turbo_thread_join(&consumer_thread), TURBO_OK);
    turbo_thread_destroy(&consumer_thread);
  }

  bench("TurboUtils SPSC fan-in upper bound") {
    uint8_t storage[BENCH_POST_PRODUCERS][BENCH_SPSC_LANE_STORAGE_SIZE];
    ring_spsc_t rings[BENCH_POST_PRODUCERS];
    spsc_fan_in_consumer_state_t consumer_state;
    spsc_lane_producer_state_t producer_states[BENCH_POST_PRODUCERS];
    turbo_thread_t consumer_thread;
    turbo_thread_t producers[BENCH_POST_PRODUCERS];
    atomic_int consumed;
    atomic_int epoch;
    atomic_int producer_done;
    atomic_int error;
    atomic_int stop;
    int rc = TURBO_OK;

    atomic_init(&consumed, 0);
    atomic_init(&epoch, 0);
    atomic_init(&producer_done, 0);
    atomic_init(&error, TURBO_OK);
    atomic_init(&stop, 0);
    for (int i = 0; i < BENCH_POST_PRODUCERS; ++i) {
      check_true(ring_spsc_init(&rings[i], storage[i], sizeof(storage[i])));
    }

    consumer_state = (spsc_fan_in_consumer_state_t){rings, &consumed, &error, &stop};
    check_int_eq(turbo_thread_create(&consumer_thread, spsc_fan_in_consumer, &consumer_state),
                 TURBO_OK);
    for (int i = 0; i < BENCH_POST_PRODUCERS; ++i) {
      producer_states[i] = (spsc_lane_producer_state_t){&rings[i], &epoch, &producer_done, &stop};
      check_int_eq(turbo_thread_create(&producers[i], spsc_lane_producer, &producer_states[i]),
                   TURBO_OK);
    }

    benchmark_titles("benchmark", "items", "items", "avg/item(us)", NULL, "min_batch(us)",
                     "max_batch(us)", "items/s", NULL, NULL);

    benchmark("ring_spsc_4lane_8192_busy_poll", 20, BENCH_POST_BATCH) {
      atomic_store_explicit(&consumed, 0, memory_order_release);
      atomic_store_explicit(&producer_done, 0, memory_order_release);
      atomic_store_explicit(&error, TURBO_OK, memory_order_release);
      atomic_fetch_add_explicit(&epoch, 1, memory_order_release);

      rc = wait_for_atomic_value(&producer_done, BENCH_POST_PRODUCERS);
      if (rc == TURBO_OK) rc = atomic_load_explicit(&error, memory_order_acquire);
      if (rc == TURBO_OK) rc = wait_for_atomic_value(&consumed, BENCH_POST_BATCH);
      g_context_bench_sink = atomic_load_explicit(&consumed, memory_order_relaxed);
    }
    check_int_eq(rc, TURBO_OK);
    check_int_eq(atomic_load_explicit(&consumed, memory_order_acquire), BENCH_POST_BATCH);

    atomic_store_explicit(&stop, 1, memory_order_release);
    atomic_fetch_add_explicit(&epoch, 1, memory_order_release);
    for (int i = 0; i < BENCH_POST_PRODUCERS; ++i) {
      check_int_eq(turbo_thread_join(&producers[i]), TURBO_OK);
      turbo_thread_destroy(&producers[i]);
    }
    check_int_eq(turbo_thread_join(&consumer_thread), TURBO_OK);
    turbo_thread_destroy(&consumer_thread);
  }

  bench("TurboUtils Disruptor MPSC upper bound") {
    const disruptor_config_t config = {sizeof(uint64_t), BENCH_POST_BATCH * 2U, 1,
                                       DISRUPTOR_MODE_BROADCAST};
    disruptor_t *queue = disruptor_create(&config);
    disruptor_consumer_state_t consumer_state;
    disruptor_producer_state_t producer_states[BENCH_POST_PRODUCERS];
    turbo_thread_t consumer_thread;
    turbo_thread_t producers[BENCH_POST_PRODUCERS];
    atomic_int consumed;
    atomic_int epoch;
    atomic_int producer_done;
    atomic_int error;
    atomic_int stop;
    int rc = TURBO_OK;
    check_not_null(queue);

    atomic_init(&consumed, 0);
    atomic_init(&epoch, 0);
    atomic_init(&producer_done, 0);
    atomic_init(&error, TURBO_OK);
    atomic_init(&stop, 0);
    consumer_state.queue = queue;
    consumer_state.next_sequence = disruptor_consumer_register(queue, &consumer_state.consumer);
    consumer_state.consumed = &consumed;
    consumer_state.stop = &stop;
    check_int_eq(turbo_thread_create(&consumer_thread, disruptor_consumer, &consumer_state),
                 TURBO_OK);

    benchmark_titles("benchmark", "items", "items", "avg/item(us)", NULL, "min_batch(us)",
                     "max_batch(us)", "items/s", NULL, NULL);

    benchmark("disruptor_1p_blocking_8192_busy_poll", 20, BENCH_POST_BATCH) {
      atomic_store_explicit(&consumed, 0, memory_order_release);
      for (int i = 0; i < BENCH_POST_BATCH; ++i) {
        rc = disruptor_publish_item(queue, (uint64_t)i);
        if (rc != TURBO_OK) break;
      }
      if (rc == TURBO_OK) rc = wait_for_atomic_value(&consumed, BENCH_POST_BATCH);
      g_context_bench_sink = atomic_load_explicit(&consumed, memory_order_relaxed);
    }
    check_int_eq(rc, TURBO_OK);
    check_int_eq(atomic_load_explicit(&consumed, memory_order_acquire), BENCH_POST_BATCH);

    for (int i = 0; i < BENCH_POST_PRODUCERS; ++i) {
      producer_states[i] =
          (disruptor_producer_state_t){queue, &epoch, &producer_done, &error, &stop};
      check_int_eq(turbo_thread_create(&producers[i], disruptor_producer, &producer_states[i]),
                   TURBO_OK);
    }

    benchmark("disruptor_4p_blocking_8192_busy_poll", 20, BENCH_POST_BATCH) {
      atomic_store_explicit(&consumed, 0, memory_order_release);
      atomic_store_explicit(&producer_done, 0, memory_order_release);
      atomic_store_explicit(&error, TURBO_OK, memory_order_release);
      atomic_fetch_add_explicit(&epoch, 1, memory_order_release);

      rc = wait_for_atomic_value(&producer_done, BENCH_POST_PRODUCERS);
      if (rc == TURBO_OK) rc = atomic_load_explicit(&error, memory_order_acquire);
      if (rc == TURBO_OK) rc = wait_for_atomic_value(&consumed, BENCH_POST_BATCH);
      g_context_bench_sink = atomic_load_explicit(&consumed, memory_order_relaxed);
    }
    check_int_eq(rc, TURBO_OK);
    check_int_eq(atomic_load_explicit(&consumed, memory_order_acquire), BENCH_POST_BATCH);

    atomic_store_explicit(&stop, 1, memory_order_release);
    atomic_fetch_add_explicit(&epoch, 1, memory_order_release);
    for (int i = 0; i < BENCH_POST_PRODUCERS; ++i) {
      check_int_eq(turbo_thread_join(&producers[i]), TURBO_OK);
      turbo_thread_destroy(&producers[i]);
    }
    check_int_eq(turbo_thread_join(&consumer_thread), TURBO_OK);
    turbo_thread_destroy(&consumer_thread);
    disruptor_consumer_unregister(queue, &consumer_state.consumer);
    disruptor_destroy(queue);
  }

  bench("sleeping context wake latency") {
    coro_context_t *ctx = coro_context_create(NULL);
    turbo_thread_t context_thread;
    wake_latency_state_t state;
    int rc = TURBO_OK;
    check_not_null(ctx);
    atomic_init(&state.started_ns, 0);
    atomic_init(&state.completed, 0);
    coro_context_set_persistent(ctx, 1);
    check_int_eq(turbo_thread_create(&context_thread, context_runner, ctx), TURBO_OK);

    for (int i = 0; i < BENCH_WAKE_SAMPLES; ++i) {
      /* Let the persistent context return to its blocking poll so this measures
       * the empty-to-nonempty wake path rather than an already-running loop. */
      turbo_sleep_ms(BENCH_WAKE_IDLE_MS);
      atomic_store_explicit(&state.started_ns, turbo_hrtime(), memory_order_release);
      rc = coro_post(ctx, record_wake_latency, &state, NULL);
      if (rc != TURBO_OK) break;
      rc = wait_for_atomic_value(&state.completed, i + 1);
      if (rc != TURBO_OK) break;
    }

    if (rc == TURBO_OK) {
      qsort(state.samples_ns, BENCH_WAKE_SAMPLES, sizeof(state.samples_ns[0]), compare_u64);
      printf("      sleeping_loop_wake_ns p50=%llu p95=%llu p99=%llu max=%llu\n",
             (unsigned long long)percentile_nearest_rank(state.samples_ns, BENCH_WAKE_SAMPLES, 50),
             (unsigned long long)percentile_nearest_rank(state.samples_ns, BENCH_WAKE_SAMPLES, 95),
             (unsigned long long)percentile_nearest_rank(state.samples_ns, BENCH_WAKE_SAMPLES, 99),
             (unsigned long long)state.samples_ns[BENCH_WAKE_SAMPLES - 1]);
    }
    check_int_eq(rc, TURBO_OK);

    coro_context_set_persistent(ctx, 0);
    coro_context_stop(ctx);
    check_int_eq(turbo_thread_join(&context_thread), TURBO_OK);
    turbo_thread_destroy(&context_thread);
    coro_context_destroy(ctx);
  }

  it("preserves callback counts across the post benchmark path") {
    coro_context_t *ctx = coro_context_create(NULL);
    atomic_int callback_count;
    check_not_null(ctx);
    atomic_init(&callback_count, 0);

    for (int i = 0; i < 16; ++i) {
      check_int_eq(coro_post(ctx, count_post, (void *)&callback_count, NULL), TURBO_OK);
    }
    (void)coro_context_run(ctx, TURBO_RUN_NOWAIT);
    check_int_eq(atomic_load_explicit(&callback_count, memory_order_acquire), 16);
    coro_context_destroy(ctx);
  }
}
