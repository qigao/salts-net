#include "disruptor.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <intrin.h>
  #include <malloc.h>
  #include <windows.h>
#else
  #include <sched.h>
#endif

#define DISRUPTOR_CACHE_LINE_SIZE 64U
#define DISRUPTOR_PAGE_SIZE 4096U

#define DISRUPTOR_WAIT_COUNT 256U
#define DISRUPTOR_YIELD_INTERVAL 1024U
#define DISRUPTOR_PRODUCER_YIELD_INTERVAL 64U
#define DISRUPTOR_VACANT UINT_FAST64_MAX

#if defined(__GNUC__) || defined(__clang__)
  #define DISRUPTOR_LIKELY(x) __builtin_expect(!!(x), 1)
  #define DISRUPTOR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
  #define DISRUPTOR_LIKELY(x) (x)
  #define DISRUPTOR_UNLIKELY(x) (x)
#endif

#ifdef _MSC_VER
__declspec(align(DISRUPTOR_CACHE_LINE_SIZE)) struct disruptor_count_s {
#else
struct disruptor_count_s {
#endif
  uint_fast64_t count;
#ifndef _MSC_VER
} __attribute__((aligned(DISRUPTOR_CACHE_LINE_SIZE)));
#else
};
#endif
typedef struct disruptor_count_s disruptor_count_t;

#ifdef _MSC_VER
__declspec(align(DISRUPTOR_CACHE_LINE_SIZE)) struct disruptor_cursor_state_s {
#else
struct disruptor_cursor_state_s {
#endif
  atomic_uint_fast64_t sequence;
#ifndef _MSC_VER
} __attribute__((aligned(DISRUPTOR_CACHE_LINE_SIZE)));
#else
};
#endif
typedef struct disruptor_cursor_state_s disruptor_cursor_state_t;

struct disruptor_s {
  disruptor_count_t reduced_size;
  disruptor_cursor_state_t slowest_consumer;
  disruptor_cursor_state_t max_read_cursor;
  disruptor_cursor_state_t write_cursor;
  uint64_t capacity;
  uint32_t consumer_capacity;
  size_t entry_size;
  disruptor_cursor_state_t *consumer_cursors;
  atomic_uint_fast64_t *published_sequences;
  uint8_t *buffer;
};

static int disruptor_is_power_of_two(uint64_t value) {
  return value != 0U && (value & (value - 1U)) == 0U;
}

static void disruptor_cpu_pause(void) {
#if defined(_MSC_VER)
  _mm_pause();
#elif defined(__GNUC__) || defined(__clang__)
  #if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
  #elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
  #endif
#endif
}

static void disruptor_thread_yield(void) {
#ifdef _WIN32
  SwitchToThread();
#else
  sched_yield();
#endif
}

static void disruptor_spin_pause(void) {
  unsigned int i;
  for (i = 0; i < DISRUPTOR_WAIT_COUNT; ++i) {
    disruptor_cpu_pause();
  }
}

static void disruptor_spin_backoff(unsigned int *wait_rounds) {
  if (++(*wait_rounds) >= DISRUPTOR_YIELD_INTERVAL) {
    *wait_rounds = 0U;
    disruptor_thread_yield();
    return;
  }
  disruptor_spin_pause();
}

static void disruptor_spin_backoff_producer(unsigned int *wait_rounds) {
  if (++(*wait_rounds) >= DISRUPTOR_PRODUCER_YIELD_INTERVAL) {
    *wait_rounds = 0U;
    disruptor_thread_yield();
    return;
  }
  disruptor_spin_pause();
}

static void *disruptor_aligned_malloc(size_t alignment, size_t size) {
#ifdef _WIN32
  return _aligned_malloc(size, alignment);
#else
  void *ptr = NULL;
  if (posix_memalign(&ptr, alignment, size) != 0) {
    return NULL;
  }
  return ptr;
#endif
}

static void disruptor_aligned_free(void *ptr) {
#ifdef _WIN32
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

static uint64_t disruptor_ring_index(const disruptor_t *disruptor, uint64_t sequence) {
  return disruptor->reduced_size.count & sequence;
}

static int disruptor_publisher_has_capacity(const disruptor_t *disruptor, uint64_t writer_sequence,
                                            uint64_t slowest_sequence) {
  return (writer_sequence - slowest_sequence) <= disruptor->reduced_size.count;
}

static uint64_t disruptor_refresh_slowest_reader(disruptor_t *disruptor, uint64_t target_sequence) {
  uint32_t i;
  uint64_t slowest_sequence = DISRUPTOR_VACANT;
  uint64_t cached_sequence;

  for (i = 0; i < disruptor->consumer_capacity; ++i) {
    uint64_t seq =
        atomic_load_explicit(&disruptor->consumer_cursors[i].sequence, memory_order_acquire);
    if (seq < slowest_sequence) {
      slowest_sequence = seq;
    }
  }

  if (DISRUPTOR_UNLIKELY(slowest_sequence == DISRUPTOR_VACANT)) {
    slowest_sequence = target_sequence - disruptor_ring_index(disruptor, target_sequence);
  }

  cached_sequence =
      atomic_load_explicit(&disruptor->slowest_consumer.sequence, memory_order_relaxed);
  if (slowest_sequence > cached_sequence) {
    atomic_store_explicit(&disruptor->slowest_consumer.sequence, slowest_sequence,
                          memory_order_relaxed);
  }

  return slowest_sequence;
}

static void disruptor_mark_published(disruptor_t *disruptor, uint64_t sequence) {
  uint64_t index = disruptor_ring_index(disruptor, sequence);
  atomic_store_explicit(&disruptor->published_sequences[index], sequence, memory_order_release);
}

static uint64_t disruptor_try_advance_published_cursor(disruptor_t *disruptor) {
  while (1) {
    uint64_t current =
        atomic_load_explicit(&disruptor->max_read_cursor.sequence, memory_order_relaxed);
    uint64_t next = current + 1U;
    uint64_t probe = next;

    while (atomic_load_explicit(
               &disruptor->published_sequences[disruptor_ring_index(disruptor, probe)],
               memory_order_acquire) == probe) {
      ++probe;
    }

    if (probe == next) {
      return current;
    }

    {
      uint64_t desired = probe - 1U;
      uint64_t expected = current;
      if (atomic_compare_exchange_weak_explicit(&disruptor->max_read_cursor.sequence, &expected,
                                                desired, memory_order_release,
                                                memory_order_relaxed)) {
        return desired;
      }
    }
  }
}

static void disruptor_init(disruptor_t *disruptor) {
  uint32_t i;
  uint64_t s;

  for (i = 0; i < disruptor->consumer_capacity; ++i) {
    atomic_store_explicit(&disruptor->consumer_cursors[i].sequence, DISRUPTOR_VACANT,
                          memory_order_relaxed);
  }
  for (s = 0; s < disruptor->capacity; ++s) {
    atomic_store_explicit(&disruptor->published_sequences[s], 0U, memory_order_relaxed);
  }

  disruptor->reduced_size.count = disruptor->capacity - 1U;
  atomic_store_explicit(&disruptor->slowest_consumer.sequence, 0U, memory_order_relaxed);
  atomic_store_explicit(&disruptor->max_read_cursor.sequence, 0U, memory_order_relaxed);
  atomic_store_explicit(&disruptor->write_cursor.sequence, 0U, memory_order_relaxed);
}

disruptor_t *disruptor_create(const disruptor_config_t *config) {
  disruptor_t *disruptor;
  size_t buffer_bytes;
  size_t cursors_bytes;
  size_t published_bytes;

  if (config == NULL) {
    return NULL;
  }
  if (config->entry_size == 0U || config->capacity == 0U || config->consumer_capacity == 0U) {
    return NULL;
  }
  if (!disruptor_is_power_of_two(config->capacity)) {
    return NULL;
  }
  if (config->entry_size > (SIZE_MAX / config->capacity)) {
    return NULL;
  }
  if (config->capacity > ((uint64_t)SIZE_MAX / sizeof(atomic_uint_fast64_t))) {
    return NULL;
  }

  buffer_bytes = config->entry_size * (size_t)config->capacity;
  cursors_bytes = sizeof(disruptor_cursor_state_t) * config->consumer_capacity;
  published_bytes = sizeof(atomic_uint_fast64_t) * (size_t)config->capacity;

  disruptor = (disruptor_t *)disruptor_aligned_malloc(DISRUPTOR_PAGE_SIZE, sizeof(*disruptor));
  if (disruptor == NULL) {
    return NULL;
  }
  memset(disruptor, 0, sizeof(*disruptor));

  disruptor->consumer_cursors = (disruptor_cursor_state_t *)disruptor_aligned_malloc(
      DISRUPTOR_CACHE_LINE_SIZE, cursors_bytes);
  if (disruptor->consumer_cursors == NULL) {
    disruptor_aligned_free(disruptor);
    return NULL;
  }
  disruptor->published_sequences =
      (atomic_uint_fast64_t *)disruptor_aligned_malloc(DISRUPTOR_CACHE_LINE_SIZE, published_bytes);
  if (disruptor->published_sequences == NULL) {
    disruptor_aligned_free(disruptor->consumer_cursors);
    disruptor_aligned_free(disruptor);
    return NULL;
  }

  disruptor->buffer = (uint8_t *)disruptor_aligned_malloc(DISRUPTOR_CACHE_LINE_SIZE, buffer_bytes);
  if (disruptor->buffer == NULL) {
    disruptor_aligned_free((void *)disruptor->published_sequences);
    disruptor_aligned_free(disruptor->consumer_cursors);
    disruptor_aligned_free(disruptor);
    return NULL;
  }

  disruptor->entry_size = config->entry_size;
  disruptor->capacity = config->capacity;
  disruptor->consumer_capacity = config->consumer_capacity;
  disruptor_init(disruptor);

  return disruptor;
}

void disruptor_destroy(disruptor_t *disruptor) {
  if (disruptor == NULL) {
    return;
  }
  disruptor_aligned_free(disruptor->buffer);
  disruptor_aligned_free((void *)disruptor->published_sequences);
  disruptor_aligned_free(disruptor->consumer_cursors);
  disruptor_aligned_free(disruptor);
}

int disruptor_reset(disruptor_t *disruptor) {
  if (disruptor == NULL) {
    return 0;
  }
  disruptor_init(disruptor);
  return 1;
}

uint64_t disruptor_capacity(const disruptor_t *disruptor) {
  if (disruptor == NULL) {
    return 0U;
  }
  return disruptor->capacity;
}

size_t disruptor_entry_size(const disruptor_t *disruptor) {
  if (disruptor == NULL) {
    return 0U;
  }
  return disruptor->entry_size;
}

void *disruptor_acquire_entry(disruptor_t *disruptor, const disruptor_cursor_t *cursor) {
  uint64_t index;
  if (disruptor == NULL || cursor == NULL) {
    return NULL;
  }
  index = disruptor_ring_index(disruptor, cursor->sequence);
  return disruptor->buffer + (index * disruptor->entry_size);
}

const void *disruptor_show_entry(const disruptor_t *disruptor, const disruptor_cursor_t *cursor) {
  uint64_t index;
  if (disruptor == NULL || cursor == NULL) {
    return NULL;
  }
  index = disruptor_ring_index(disruptor, cursor->sequence);
  return disruptor->buffer + (index * disruptor->entry_size);
}

int disruptor_consumer_try_register(disruptor_t *disruptor, disruptor_consumer_t *consumer,
                                    uint64_t *next_sequence) {
  uint32_t i;
  uint64_t vacant;
  uint64_t start_sequence;

  if (disruptor == NULL || consumer == NULL) {
    return 0;
  }

  for (i = 0; i < disruptor->consumer_capacity; ++i) {
    vacant = DISRUPTOR_VACANT;
    start_sequence =
        atomic_load_explicit(&disruptor->slowest_consumer.sequence, memory_order_acquire);
    if (atomic_compare_exchange_weak_explicit(&disruptor->consumer_cursors[i].sequence, &vacant,
                                              start_sequence, memory_order_release,
                                              memory_order_relaxed)) {
      consumer->slot = i;
      if (start_sequence == 0U) {
        start_sequence = 1U;
        atomic_store_explicit(&disruptor->consumer_cursors[i].sequence, start_sequence,
                              memory_order_release);
      }
      if (next_sequence != NULL) {
        *next_sequence = start_sequence;
      }
      return 1;
    }
  }

  return 0;
}

uint64_t disruptor_consumer_register(disruptor_t *disruptor, disruptor_consumer_t *consumer) {
  unsigned int wait_rounds = 0U;
  uint64_t next_sequence = 0U;

  if (disruptor == NULL || consumer == NULL) {
    return 0U;
  }

  while (!disruptor_consumer_try_register(disruptor, consumer, &next_sequence)) {
    disruptor_spin_backoff(&wait_rounds);
  }
  return next_sequence;
}

void disruptor_consumer_unregister(disruptor_t *disruptor, const disruptor_consumer_t *consumer) {
  if (disruptor == NULL || consumer == NULL || consumer->slot >= disruptor->consumer_capacity) {
    return;
  }

  atomic_store_explicit(&disruptor->consumer_cursors[consumer->slot].sequence, DISRUPTOR_VACANT,
                        memory_order_release);
  disruptor_refresh_slowest_reader(
      disruptor,
      1U + atomic_load_explicit(&disruptor->write_cursor.sequence, memory_order_relaxed));
}

int disruptor_consumer_wait_for_nonblocking(const disruptor_t *disruptor,
                                            disruptor_cursor_t *cursor) {
  uint64_t required_sequence;
  if (disruptor == NULL || cursor == NULL) {
    return 0;
  }

  required_sequence = cursor->sequence;
  if (required_sequence >
      atomic_load_explicit(&disruptor->max_read_cursor.sequence, memory_order_relaxed)) {
    return 0;
  }

  cursor->sequence =
      atomic_load_explicit(&disruptor->max_read_cursor.sequence, memory_order_acquire);
  return 1;
}

void disruptor_consumer_wait_for_blocking(const disruptor_t *disruptor,
                                          disruptor_cursor_t *cursor) {
  uint64_t required_sequence;
  unsigned int wait_rounds = 0U;

  if (disruptor == NULL || cursor == NULL) {
    return;
  }

  required_sequence = cursor->sequence;
  while (required_sequence >
         atomic_load_explicit(&disruptor->max_read_cursor.sequence, memory_order_relaxed)) {
    disruptor_spin_backoff(&wait_rounds);
  }

  cursor->sequence =
      atomic_load_explicit(&disruptor->max_read_cursor.sequence, memory_order_acquire);
}

void disruptor_consumer_release_entry(disruptor_t *disruptor, const disruptor_consumer_t *consumer,
                                      const disruptor_cursor_t *cursor) {
  if (disruptor == NULL || consumer == NULL || cursor == NULL ||
      consumer->slot >= disruptor->consumer_capacity) {
    return;
  }

  atomic_store_explicit(&disruptor->consumer_cursors[consumer->slot].sequence, cursor->sequence,
                        memory_order_release);
}

static int disruptor_range_is_valid(const disruptor_sequence_range_t *range) {
  return range != NULL && range->first_sequence != 0U &&
         range->last_sequence >= range->first_sequence;
}

static int disruptor_try_claim_range_internal(disruptor_t *disruptor, uint32_t count,
                                              disruptor_sequence_range_t *range) {
  uint64_t writer_last;
  uint64_t first_sequence;
  uint64_t last_sequence;
  uint64_t slowest_sequence;
  uint64_t expected;

  if (disruptor == NULL || range == NULL || count == 0U) {
    return 0;
  }
  if ((uint64_t)count > disruptor->reduced_size.count) {
    return 0;
  }

  writer_last = atomic_load_explicit(&disruptor->write_cursor.sequence, memory_order_relaxed);
  first_sequence = writer_last + 1U;
  if (first_sequence == 0U || first_sequence > (UINT64_MAX - ((uint64_t)count - 1U))) {
    return 0;
  }
  last_sequence = first_sequence + ((uint64_t)count - 1U);

  slowest_sequence =
      atomic_load_explicit(&disruptor->slowest_consumer.sequence, memory_order_acquire);
  if (!disruptor_publisher_has_capacity(disruptor, last_sequence, slowest_sequence)) {
    slowest_sequence = disruptor_refresh_slowest_reader(disruptor, last_sequence);
    if (!disruptor_publisher_has_capacity(disruptor, last_sequence, slowest_sequence)) {
      return 0;
    }
  }

  expected = writer_last;
  if (!atomic_compare_exchange_strong_explicit(&disruptor->write_cursor.sequence, &expected,
                                               writer_last + (uint64_t)count, memory_order_relaxed,
                                               memory_order_relaxed)) {
    return 0;
  }

  range->first_sequence = first_sequence;
  range->last_sequence = last_sequence;
  return 1;
}

static int disruptor_claim_range_blocking_internal(disruptor_t *disruptor, uint32_t count,
                                                   disruptor_sequence_range_t *range) {
  uint64_t first_sequence;
  uint64_t last_sequence;
  unsigned int wait_rounds = 0U;

  if (disruptor == NULL || range == NULL || count == 0U) {
    return 0;
  }
  if ((uint64_t)count > disruptor->reduced_size.count) {
    return 0;
  }

  first_sequence = 1U + atomic_fetch_add_explicit(&disruptor->write_cursor.sequence,
                                                  (uint64_t)count, memory_order_relaxed);
  if (first_sequence == 0U || first_sequence > (UINT64_MAX - ((uint64_t)count - 1U))) {
    return 0;
  }
  last_sequence = first_sequence + ((uint64_t)count - 1U);

  while (1) {
    uint64_t slowest_sequence =
        atomic_load_explicit(&disruptor->slowest_consumer.sequence, memory_order_acquire);
    if (DISRUPTOR_LIKELY(
            disruptor_publisher_has_capacity(disruptor, last_sequence, slowest_sequence))) {
      range->first_sequence = first_sequence;
      range->last_sequence = last_sequence;
      return 1;
    }

    if ((wait_rounds & 15U) == 0U) {
      slowest_sequence = disruptor_refresh_slowest_reader(disruptor, last_sequence);
      if (DISRUPTOR_LIKELY(
              disruptor_publisher_has_capacity(disruptor, last_sequence, slowest_sequence))) {
        range->first_sequence = first_sequence;
        range->last_sequence = last_sequence;
        return 1;
      }
    }

    disruptor_spin_backoff_producer(&wait_rounds);
  }
}

static int disruptor_publish_range_internal(disruptor_t *disruptor,
                                            const disruptor_sequence_range_t *range,
                                            int wait_until_visible, int report_visibility) {
  uint64_t expected;
  uint64_t sequence;
  unsigned int wait_rounds = 0U;

  if (disruptor == NULL || !disruptor_range_is_valid(range)) {
    return 0;
  }

  expected = range->first_sequence - 1U;
  if (atomic_compare_exchange_strong_explicit(&disruptor->max_read_cursor.sequence, &expected,
                                              range->last_sequence, memory_order_release,
                                              memory_order_relaxed)) {
    return 1;
  }

  sequence = range->first_sequence;
  while (1) {
    disruptor_mark_published(disruptor, sequence);
    if (sequence == range->last_sequence) {
      break;
    }
    ++sequence;
  }

  if (wait_until_visible) {
    while (atomic_load_explicit(&disruptor->max_read_cursor.sequence, memory_order_acquire) <
           range->last_sequence) {
      (void)disruptor_try_advance_published_cursor(disruptor);
      disruptor_spin_backoff_producer(&wait_rounds);
    }
    return 1;
  }

  (void)disruptor_try_advance_published_cursor(disruptor);
  if (report_visibility) {
    return atomic_load_explicit(&disruptor->max_read_cursor.sequence, memory_order_acquire) >=
           range->last_sequence;
  }

  return 1;
}

int disruptor_publisher_try_claim(disruptor_t *disruptor, disruptor_cursor_t *cursor) {
  disruptor_sequence_range_t range;

  if (cursor == NULL) {
    return 0;
  }
  if (!disruptor_try_claim_range_internal(disruptor, 1U, &range)) {
    return 0;
  }
  cursor->sequence = range.first_sequence;
  return 1;
}

int disruptor_publisher_try_claim_n(disruptor_t *disruptor, uint32_t count,
                                    disruptor_sequence_range_t *range) {
  return disruptor_try_claim_range_internal(disruptor, count, range);
}

void disruptor_publisher_next_entry_blocking(disruptor_t *disruptor, disruptor_cursor_t *cursor) {
  disruptor_sequence_range_t range;
  if (cursor == NULL) {
    return;
  }
  if (!disruptor_claim_range_blocking_internal(disruptor, 1U, &range)) {
    return;
  }
  cursor->sequence = range.first_sequence;
}

int disruptor_publisher_claim_n_blocking(disruptor_t *disruptor, uint32_t count,
                                         disruptor_sequence_range_t *range) {
  return disruptor_claim_range_blocking_internal(disruptor, count, range);
}

void *disruptor_publisher_next_entry_and_acquire_blocking(disruptor_t *disruptor,
                                                          disruptor_cursor_t *cursor) {
  disruptor_sequence_range_t range;
  uint64_t index;

  if (disruptor == NULL || cursor == NULL) {
    return NULL;
  }
  if (!disruptor_claim_range_blocking_internal(disruptor, 1U, &range)) {
    return NULL;
  }

  cursor->sequence = range.first_sequence;
  index = disruptor_ring_index(disruptor, range.first_sequence);
  return disruptor->buffer + (index * disruptor->entry_size);
}

int disruptor_publisher_try_commit(disruptor_t *disruptor, const disruptor_cursor_t *cursor) {
  disruptor_sequence_range_t range;
  if (cursor == NULL || cursor->sequence == 0U) {
    return 0;
  }
  range.first_sequence = cursor->sequence;
  range.last_sequence = cursor->sequence;
  return disruptor_publish_range_internal(disruptor, &range, 0, 1);
}

void disruptor_publisher_commit_entry_blocking(disruptor_t *disruptor,
                                               const disruptor_cursor_t *cursor) {
  disruptor_sequence_range_t range;
  if (cursor == NULL || cursor->sequence == 0U) {
    return;
  }
  range.first_sequence = cursor->sequence;
  range.last_sequence = cursor->sequence;
  (void)disruptor_publish_range_internal(disruptor, &range, 1, 0);
}

int disruptor_publisher_publish_range(disruptor_t *disruptor,
                                      const disruptor_sequence_range_t *range) {
  return disruptor_publish_range_internal(disruptor, range, 0, 0);
}

void disruptor_publisher_commit_range_blocking(disruptor_t *disruptor,
                                               const disruptor_sequence_range_t *range) {
  (void)disruptor_publish_range_internal(disruptor, range, 1, 0);
}

int disruptor_publisher_publish(disruptor_t *disruptor, const disruptor_cursor_t *cursor) {
  disruptor_sequence_range_t range;
  if (cursor == NULL || cursor->sequence == 0U) {
    return 0;
  }
  range.first_sequence = cursor->sequence;
  range.last_sequence = cursor->sequence;
  return disruptor_publish_range_internal(disruptor, &range, 0, 0);
}
