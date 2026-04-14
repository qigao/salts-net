#ifndef DISRUPTOR_H
#define DISRUPTOR_H

#include "platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct disruptor_s disruptor_t;

typedef struct {
  uint64_t sequence;
} disruptor_cursor_t;

typedef struct {
  uint64_t first_sequence;
  uint64_t last_sequence;
} disruptor_sequence_range_t;

typedef struct {
  uint32_t slot;
} disruptor_consumer_t;

typedef struct {
  size_t entry_size;
  uint64_t capacity;
  uint32_t consumer_capacity;
} disruptor_config_t;

CXX_C_API disruptor_t *disruptor_create(const disruptor_config_t *config);
CXX_C_API void disruptor_destroy(disruptor_t *disruptor);
CXX_C_API int disruptor_reset(disruptor_t *disruptor);

CXX_C_API uint64_t disruptor_capacity(const disruptor_t *disruptor);
CXX_C_API size_t disruptor_entry_size(const disruptor_t *disruptor);

CXX_C_API void *disruptor_acquire_entry(disruptor_t *disruptor, const disruptor_cursor_t *cursor);
CXX_C_API const void *disruptor_show_entry(const disruptor_t *disruptor,
                                           const disruptor_cursor_t *cursor);

CXX_C_API int disruptor_publisher_try_claim(disruptor_t *disruptor, disruptor_cursor_t *cursor);
CXX_C_API int disruptor_publisher_try_claim_n(disruptor_t *disruptor, uint32_t count,
                                              disruptor_sequence_range_t *range);
CXX_C_API void disruptor_publisher_next_entry_blocking(disruptor_t *disruptor,
                                                       disruptor_cursor_t *cursor);
CXX_C_API void *disruptor_publisher_next_entry_and_acquire_blocking(disruptor_t *disruptor,
                                                                    disruptor_cursor_t *cursor);
CXX_C_API int disruptor_publisher_claim_n_blocking(disruptor_t *disruptor, uint32_t count,
                                                   disruptor_sequence_range_t *range);
CXX_C_API int disruptor_publisher_try_commit(disruptor_t *disruptor,
                                             const disruptor_cursor_t *cursor);
CXX_C_API void disruptor_publisher_commit_entry_blocking(disruptor_t *disruptor,
                                                         const disruptor_cursor_t *cursor);
CXX_C_API int disruptor_publisher_publish_range(disruptor_t *disruptor,
                                                const disruptor_sequence_range_t *range);
CXX_C_API void disruptor_publisher_commit_range_blocking(disruptor_t *disruptor,
                                                         const disruptor_sequence_range_t *range);
CXX_C_API int disruptor_publisher_publish(disruptor_t *disruptor, const disruptor_cursor_t *cursor);

CXX_C_API int disruptor_consumer_try_register(disruptor_t *disruptor,
                                              disruptor_consumer_t *consumer,
                                              uint64_t *next_sequence);
CXX_C_API uint64_t disruptor_consumer_register(disruptor_t *disruptor,
                                               disruptor_consumer_t *consumer);
CXX_C_API void disruptor_consumer_unregister(disruptor_t *disruptor,
                                             const disruptor_consumer_t *consumer);

CXX_C_API int disruptor_consumer_wait_for_nonblocking(const disruptor_t *disruptor,
                                                      disruptor_cursor_t *cursor);
CXX_C_API void disruptor_consumer_wait_for_blocking(const disruptor_t *disruptor,
                                                    disruptor_cursor_t *cursor);
CXX_C_API void disruptor_consumer_release_entry(disruptor_t *disruptor,
                                                const disruptor_consumer_t *consumer,
                                                const disruptor_cursor_t *cursor);

/**
 * @brief Callback: return non-zero while the consumer should keep running.
 */
typedef int (*disruptor_should_run_fn)(void *ctx);

/**
 * @brief Callback: process entries in range [first_seq, last_seq].
 */
typedef void (*disruptor_batch_fn)(void *ctx, uint64_t first_seq, uint64_t last_seq);

/**
 * @brief Generic consumer loop.
 *
 * Handles register, poll-wait-process-release loop, shutdown drain, and
 * unregister.  Callers only supply two callbacks: whether to keep running,
 * and how to process a batch of entries.
 */
CXX_C_API void disruptor_consumer_run(disruptor_t *disruptor,
                                      disruptor_consumer_t *consumer,
                                      disruptor_should_run_fn should_run,
                                      disruptor_batch_fn process_batch,
                                      void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DISRUPTOR_H */
