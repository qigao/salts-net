#include "disruptor.h"
#include "tinytest.h"
#include <stdbool.h>
#include <stdint.h>


#define ENTRY_SIZE sizeof(uint64_t)
#define CAPACITY 32
#define CONSUMERS 2

spec("Disruptor Tests") {
  it("should create and destroy cleanly") {
    disruptor_config_t cfg = {
        .entry_size = ENTRY_SIZE, .capacity = CAPACITY, .consumer_capacity = CONSUMERS};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);
    check_size_eq(disruptor_capacity(d), CAPACITY);
    check_size_eq(disruptor_entry_size(d), ENTRY_SIZE);

    disruptor_destroy(d);
  }

  it("should handle invalid config gracefully") {
    disruptor_config_t cfg1 = {
        .entry_size = 0, .capacity = CAPACITY, .consumer_capacity = CONSUMERS};
    check_null(disruptor_create(&cfg1));

    disruptor_config_t cfg2 = {.entry_size = ENTRY_SIZE,
                               .capacity = 31, // Not power of two
                               .consumer_capacity = CONSUMERS};
    check_null(disruptor_create(&cfg2));
  }

  it("should publish and consume a single entry") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 16, .consumer_capacity = 1};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t c;
    uint64_t next_seq;
    check_int_eq(disruptor_consumer_try_register(d, &c, &next_seq), 1);
    check_size_eq(next_seq, 1);

    disruptor_cursor_t w_cursor;
    check_int_eq(disruptor_publisher_try_claim(d, &w_cursor), 1);
    check_size_eq(w_cursor.sequence, 1);

    uint64_t *entry = (uint64_t *)disruptor_acquire_entry(d, &w_cursor);
    check_not_null(entry);
    *entry = 42;

    check_int_eq(disruptor_publisher_publish(d, &w_cursor), 1);

    disruptor_cursor_t r_cursor = {.sequence = 1};
    check_int_eq(disruptor_consumer_wait_for_nonblocking(d, &r_cursor), 1);
    check_size_eq(r_cursor.sequence, 1);

    const uint64_t *read_entry = (const uint64_t *)disruptor_show_entry(d, &r_cursor);
    check_not_null(read_entry);
    check_size_eq(*read_entry, 42);

    disruptor_consumer_release_entry(d, &c, &r_cursor);
    disruptor_consumer_unregister(d, &c);
    disruptor_destroy(d);
  }

  it("should publish and consume multiple entries block") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint32_t), .capacity = 8, .consumer_capacity = 1};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t c;
    disruptor_consumer_register(d, &c);

    disruptor_sequence_range_t range;
    check_int_eq(disruptor_publisher_try_claim_n(d, 4, &range), 1);
    check_size_eq(range.first_sequence, 1);
    check_size_eq(range.last_sequence, 4);

    for (uint64_t seq = range.first_sequence; seq <= range.last_sequence; ++seq) {
      disruptor_cursor_t wc = {.sequence = seq};
      uint32_t *entry = (uint32_t *)disruptor_acquire_entry(d, &wc);
      *entry = (uint32_t)seq * 10;
    }

    check_int_eq(disruptor_publisher_publish_range(d, &range), 1);

    disruptor_cursor_t rc = {.sequence = 1};
    check_int_eq(disruptor_consumer_wait_for_nonblocking(d, &rc), 1);
    check_size_eq(rc.sequence, 4);

    for (uint64_t seq = 1; seq <= rc.sequence; ++seq) {
      disruptor_cursor_t cur = {.sequence = seq};
      const uint32_t *read_entry = (const uint32_t *)disruptor_show_entry(d, &cur);
      check_size_eq(*read_entry, seq * 10);
    }

    disruptor_consumer_release_entry(d, &c, &rc);
    disruptor_destroy(d);
  }
}
