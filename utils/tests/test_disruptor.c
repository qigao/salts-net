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

  it("should broadcast entries to every consumer") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 16, .consumer_capacity = 2};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t c1, c2;
    uint64_t next1 = 0, next2 = 0;
    check_int_eq(disruptor_consumer_try_register(d, &c1, &next1), 1);
    check_int_eq(disruptor_consumer_try_register(d, &c2, &next2), 1);
    check_size_eq(next1, 1);
    check_size_eq(next2, 1);

    disruptor_cursor_t w = {0};
    check_int_eq(disruptor_publisher_try_claim(d, &w), 1);
    *(uint64_t *)disruptor_acquire_entry(d, &w) = 7;
    check_int_eq(disruptor_publisher_publish(d, &w), 1);

    disruptor_cursor_t r1 = {.sequence = next1};
    disruptor_cursor_t r2 = {.sequence = next2};
    check_int_eq(disruptor_consumer_wait_for_nonblocking(d, &r1), 1);
    check_int_eq(disruptor_consumer_wait_for_nonblocking(d, &r2), 1);
    check_size_eq(*(const uint64_t *)disruptor_show_entry(d, &r1), 7);
    check_size_eq(*(const uint64_t *)disruptor_show_entry(d, &r2), 7);

    disruptor_consumer_release_entry(d, &c1, &r1);
    disruptor_consumer_release_entry(d, &c2, &r2);
    disruptor_destroy(d);
  }

  it("should assign worker-pool entries to one worker only") {
    disruptor_config_t cfg = {.entry_size = sizeof(uint64_t),
                              .capacity = 16,
                              .consumer_capacity = 1,
                              .mode = DISRUPTOR_MODE_WORKER_POOL};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_sequence_range_t range;
    check_int_eq(disruptor_publisher_try_claim_n(d, 4, &range), 1);
    for (uint64_t seq = range.first_sequence; seq <= range.last_sequence; ++seq) {
      disruptor_cursor_t w = {.sequence = seq};
      *(uint64_t *)disruptor_acquire_entry(d, &w) = seq * 100;
    }
    check_int_eq(disruptor_publisher_publish_range(d, &range), 1);

    bool seen[5] = {false};
    for (int i = 0; i < 4; ++i) {
      disruptor_cursor_t claim = {0};
      check_int_eq(disruptor_worker_try_claim(d, &claim), 1);
      check(claim.sequence >= 1 && claim.sequence <= 4);
      check(!seen[claim.sequence]);
      seen[claim.sequence] = true;
      check_size_eq(*(const uint64_t *)disruptor_show_entry(d, &claim), claim.sequence * 100);
      disruptor_worker_release_entry(d, &claim);
    }

    disruptor_cursor_t extra = {0};
    check_int_eq(disruptor_worker_try_claim(d, &extra), 0);
    for (int i = 1; i <= 4; ++i) {
      check(seen[i]);
    }

    disruptor_destroy(d);
  }

  it("should gate dependent consumers until upstream consumers release") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 16, .consumer_capacity = 3};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t branch_a, branch_b, joined;
    uint64_t next_a = 0, next_b = 0, next_joined = 0;
    check_int_eq(disruptor_consumer_try_register(d, &branch_a, &next_a), 1);
    check_int_eq(disruptor_consumer_try_register(d, &branch_b, &next_b), 1);
    check_int_eq(disruptor_consumer_try_register(d, &joined, &next_joined), 1);

    disruptor_consumer_t deps[2] = {branch_a, branch_b};
    check_int_eq(disruptor_consumer_set_dependencies(d, &joined, deps, 2), 1);

    disruptor_cursor_t w = {0};
    check_int_eq(disruptor_publisher_try_claim(d, &w), 1);
    *(uint64_t *)disruptor_acquire_entry(d, &w) = 99;
    check_int_eq(disruptor_publisher_publish(d, &w), 1);

    disruptor_cursor_t joined_cursor = {.sequence = next_joined};
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &joined, &joined_cursor), 0);

    disruptor_cursor_t a_cursor = {.sequence = next_a};
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &branch_a, &a_cursor), 1);
    disruptor_consumer_release_entry(d, &branch_a, &a_cursor);
    joined_cursor.sequence = next_joined;
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &joined, &joined_cursor), 0);

    disruptor_cursor_t b_cursor = {.sequence = next_b};
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &branch_b, &b_cursor), 1);
    disruptor_consumer_release_entry(d, &branch_b, &b_cursor);

    joined_cursor.sequence = next_joined;
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &joined, &joined_cursor), 1);
    check_size_eq(*(const uint64_t *)disruptor_show_entry(d, &joined_cursor), 99);

    disruptor_consumer_release_entry(d, &joined, &joined_cursor);
    disruptor_destroy(d);
  }

  it("should configure dependencies through topology chain") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 16, .consumer_capacity = 3};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t parse, validate, persist;
    uint64_t next_parse = disruptor_consumer_register(d, &parse);
    uint64_t next_validate = disruptor_consumer_register(d, &validate);
    uint64_t next_persist = disruptor_consumer_register(d, &persist);

    disruptor_topology_t *topology = disruptor_topology_create(d);
    check_not_null(topology);
    disruptor_stage_t s_parse = disruptor_topology_stage(topology, "parse", &parse);
    disruptor_stage_t s_validate = disruptor_topology_stage(topology, "validate", &validate);
    disruptor_stage_t s_persist = disruptor_topology_stage(topology, "persist", &persist);
    disruptor_stage_t chain[] = {s_parse, s_validate, s_persist};
    check_int_eq(disruptor_topology_chain(topology, chain, 3), 1);
    check_int_eq(disruptor_topology_commit(topology), 1);

    disruptor_cursor_t w = {0};
    check_int_eq(disruptor_publisher_try_claim(d, &w), 1);
    *(uint64_t *)disruptor_acquire_entry(d, &w) = 123;
    check_int_eq(disruptor_publisher_publish(d, &w), 1);

    disruptor_cursor_t persist_cursor = {.sequence = next_persist};
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &persist, &persist_cursor), 0);

    disruptor_cursor_t parse_cursor = {.sequence = next_parse};
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &parse, &parse_cursor), 1);
    disruptor_consumer_release_entry(d, &parse, &parse_cursor);
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &persist, &persist_cursor), 0);

    disruptor_cursor_t validate_cursor = {.sequence = next_validate};
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &validate, &validate_cursor), 1);
    disruptor_consumer_release_entry(d, &validate, &validate_cursor);

    persist_cursor.sequence = next_persist;
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &persist, &persist_cursor), 1);
    check_size_eq(*(const uint64_t *)disruptor_show_entry(d, &persist_cursor), 123);

    disruptor_topology_destroy(topology);
    disruptor_destroy(d);
  }

  it("should expand topology group dependencies") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 16, .consumer_capacity = 4};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t parse, validate, enrich, sink;
    uint64_t next_parse = disruptor_consumer_register(d, &parse);
    uint64_t next_validate = disruptor_consumer_register(d, &validate);
    uint64_t next_enrich = disruptor_consumer_register(d, &enrich);
    uint64_t next_sink = disruptor_consumer_register(d, &sink);

    disruptor_topology_t *topology = disruptor_topology_create(d);
    check_not_null(topology);
    disruptor_stage_t s_parse = disruptor_topology_stage(topology, "parse", &parse);
    disruptor_stage_t s_validate = disruptor_topology_stage(topology, "validate", &validate);
    disruptor_stage_t s_enrich = disruptor_topology_stage(topology, "enrich", &enrich);
    disruptor_stage_t s_sink = disruptor_topology_stage(topology, "sink", &sink);
    disruptor_stage_t middle_stages[] = {s_validate, s_enrich};
    disruptor_group_t middle = disruptor_topology_group(topology, "middle", middle_stages, 2);

    check_int_eq(disruptor_topology_group_after(topology, middle, s_parse), 1);
    check_int_eq(disruptor_topology_stage_after_group(topology, s_sink, middle), 1);
    check_int_eq(disruptor_topology_commit(topology), 1);

    disruptor_cursor_t w = {0};
    check_int_eq(disruptor_publisher_try_claim(d, &w), 1);
    *(uint64_t *)disruptor_acquire_entry(d, &w) = 456;
    check_int_eq(disruptor_publisher_publish(d, &w), 1);

    disruptor_cursor_t sink_cursor = {.sequence = next_sink};
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &sink, &sink_cursor), 0);

    disruptor_cursor_t parse_cursor = {.sequence = next_parse};
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &parse, &parse_cursor), 1);
    disruptor_consumer_release_entry(d, &parse, &parse_cursor);

    disruptor_cursor_t validate_cursor = {.sequence = next_validate};
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &validate, &validate_cursor), 1);
    disruptor_consumer_release_entry(d, &validate, &validate_cursor);
    sink_cursor.sequence = next_sink;
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &sink, &sink_cursor), 0);

    disruptor_cursor_t enrich_cursor = {.sequence = next_enrich};
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &enrich, &enrich_cursor), 1);
    disruptor_consumer_release_entry(d, &enrich, &enrich_cursor);

    sink_cursor.sequence = next_sink;
    check_int_eq(disruptor_consumer_wait_for_nonblocking_for(d, &sink, &sink_cursor), 1);
    check_size_eq(*(const uint64_t *)disruptor_show_entry(d, &sink_cursor), 456);

    disruptor_topology_destroy(topology);
    disruptor_destroy(d);
  }

  it("should reject topology cycles") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 16, .consumer_capacity = 2};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t a, b;
    disruptor_consumer_register(d, &a);
    disruptor_consumer_register(d, &b);

    disruptor_topology_t *topology = disruptor_topology_create(d);
    check_not_null(topology);
    disruptor_stage_t s_a = disruptor_topology_stage(topology, "a", &a);
    disruptor_stage_t s_b = disruptor_topology_stage(topology, "b", &b);
    check_int_eq(disruptor_topology_after(topology, s_a, s_b), 1);
    check_int_eq(disruptor_topology_after(topology, s_b, s_a), 1);
    check_int_eq(disruptor_topology_commit(topology), 0);

    disruptor_topology_destroy(topology);
    disruptor_destroy(d);
  }
}
