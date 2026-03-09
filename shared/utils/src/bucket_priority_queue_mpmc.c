#include "bucket_priority_queue_mpmc.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define BUCKET_ENTRY_SIZE sizeof(bucket_priority_mpmc_value_t)

static bool bucket_priority_mpmc_valid(bucket_priority_mpmc_t priority) {
  return priority >= BUCKET_PRIORITY_MPMC_LOW && priority < BUCKET_PRIORITY_MPMC_COUNT;
}

bool bucket_priority_queue_mpmc_init(bucket_priority_queue_mpmc_t *queue,
                                     size_t capacity_per_bucket,
                                     uint32_t max_consumers) {
  if (queue == NULL || capacity_per_bucket == 0) {
    return false;
  }

  memset(queue, 0, sizeof(*queue));
  queue->max_consumers = max_consumers;

  disruptor_config_t config = {
    .entry_size = BUCKET_ENTRY_SIZE,
    .capacity = capacity_per_bucket,
    .consumer_capacity = 1 /* WE ONLY NEED 1 LOGICAL CONSUMER! */
  };

  for (size_t i = 0; i < BUCKET_PRIORITY_MPMC_COUNT; ++i) {
    queue->buckets[i].disruptor = disruptor_create(&config);
    if (queue->buckets[i].disruptor == NULL) {
      // Cleanup on failure
      for (size_t j = 0; j < i; ++j) {
        disruptor_destroy(queue->buckets[j].disruptor);
      }
      return false;
    }
    
    // Register the shared logical consumer
    uint64_t initial_seq = disruptor_consumer_register(queue->buckets[i].disruptor, &queue->buckets[i].shared_consumer);
    queue->buckets[i].next_read_sequence = initial_seq;
    turbo_atomic_store_uint32(&queue->buckets[i].pop_lock, 0);
  }

  return true;
}

void bucket_priority_queue_mpmc_destroy(bucket_priority_queue_mpmc_t *queue) {
  if (queue == NULL) {
    return;
  }

  for (size_t i = 0; i < BUCKET_PRIORITY_MPMC_COUNT; ++i) {
    if (queue->buckets[i].disruptor != NULL) {
      disruptor_destroy(queue->buckets[i].disruptor);
      queue->buckets[i].disruptor = NULL;
    }
  }
}

bool bucket_priority_queue_mpmc_try_push(bucket_priority_queue_mpmc_t *queue,
                                         bucket_priority_mpmc_t priority,
                                         bucket_priority_mpmc_value_t value) {
  if (queue == NULL || !bucket_priority_mpmc_valid(priority)) {
    return false;
  }

  disruptor_t *disruptor = queue->buckets[(size_t)priority].disruptor;
  disruptor_cursor_t cursor;

  if (disruptor_publisher_try_claim(disruptor, &cursor) != 1) {
    return false;  // Queue full
  }

  bucket_priority_mpmc_value_t *entry =
      (bucket_priority_mpmc_value_t *)disruptor_acquire_entry(disruptor, &cursor);
  *entry = value;

  disruptor_publisher_publish(disruptor, &cursor);
  return true;
}

void bucket_priority_queue_mpmc_push_blocking(bucket_priority_queue_mpmc_t *queue,
                                              bucket_priority_mpmc_t priority,
                                              bucket_priority_mpmc_value_t value) {
  if (queue == NULL || !bucket_priority_mpmc_valid(priority)) {
    return;
  }

  disruptor_t *disruptor = queue->buckets[(size_t)priority].disruptor;
  disruptor_cursor_t cursor;

  bucket_priority_mpmc_value_t *entry =
      (bucket_priority_mpmc_value_t *)disruptor_publisher_next_entry_and_acquire_blocking(
          disruptor, &cursor);
  *entry = value;

  disruptor_publisher_commit_entry_blocking(disruptor, &cursor);
}

bool bucket_priority_queue_mpmc_try_pop(
    bucket_priority_queue_mpmc_t *queue,
    bucket_priority_mpmc_value_t *out_value) {
  if (queue == NULL || out_value == NULL) {
    return false;
  }

  // Poll from highest priority first (CRITICAL -> LOW)
  for (int i = BUCKET_PRIORITY_MPMC_CRITICAL; i >= BUCKET_PRIORITY_MPMC_LOW; --i) {
    bucket_priority_bucket_mpmc_t *bucket = &queue->buckets[i];
    
    // Attempt to acquire pop spinlock
    if (turbo_atomic_cas_uint32(&bucket->pop_lock, 0, 1)) {
      disruptor_t *disruptor = bucket->disruptor;
      uint64_t seq = bucket->next_read_sequence;
      disruptor_cursor_t cursor = {.sequence = seq};

      if (disruptor_consumer_wait_for_nonblocking(disruptor, &cursor) == 1) {
        // Data available at seq
        disruptor_cursor_t read_cursor = {.sequence = seq};
        const bucket_priority_mpmc_value_t *entry =
            (const bucket_priority_mpmc_value_t *)disruptor_show_entry(disruptor, &read_cursor);

        *out_value = *entry;
        
        // Update local tracking
        bucket->next_read_sequence = seq + 1;
        
        // Release entry to disruptor
        disruptor_consumer_release_entry(disruptor, &bucket->shared_consumer, &read_cursor);

        // Unlock
        turbo_atomic_store_uint32(&bucket->pop_lock, 0);
        return true;
      }
      
      // Unlock
      turbo_atomic_store_uint32(&bucket->pop_lock, 0);
    }
  }

  return false;
}

bool bucket_priority_queue_mpmc_pop_blocking(
    bucket_priority_queue_mpmc_t *queue,
    bucket_priority_mpmc_value_t *out_value,
    uint32_t timeout_ms) {
  if (queue == NULL || out_value == NULL) {
    return false;
  }

  // Simple spin with timeout (can be improved with condition variables)
  uint32_t elapsed = 0;
  const uint32_t sleep_interval = 1;  // 1ms

  while (elapsed < timeout_ms) {
    if (bucket_priority_queue_mpmc_try_pop(queue, out_value)) {
      return true;
    }

    // Sleep briefly
    #ifdef _WIN32
      Sleep(sleep_interval);
    #else
      usleep(sleep_interval * 1000);
    #endif

    elapsed += sleep_interval;
  }

  return false;
}

bool bucket_priority_queue_mpmc_empty(const bucket_priority_queue_mpmc_t *queue) {
  if (queue == NULL) {
    return true;
  }

  // Check all buckets from highest to lowest priority
  for (int i = BUCKET_PRIORITY_MPMC_CRITICAL; i >= BUCKET_PRIORITY_MPMC_LOW; --i) {
    const bucket_priority_bucket_mpmc_t *bucket = &queue->buckets[i];
    disruptor_t *disruptor = bucket->disruptor;

    if (disruptor == NULL) {
      continue;
    }

    // Check if there are unconsumed entries
    // Compare next_read_sequence with the published cursor
    uint64_t next_seq = bucket->next_read_sequence;
    disruptor_cursor_t cursor = {.sequence = next_seq};

    if (disruptor_consumer_wait_for_nonblocking(disruptor, &cursor) == 1) {
      return false;  // Found data in this bucket
    }
  }

  return true;  // All buckets are empty
}
