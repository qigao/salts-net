#include "arena_buffer.h"
#include "stats.h"
#include "tinytest.h"
#include <stdint.h>
#include <string.h>

static turbo_arena_t test_arena;

spec("Simple Arena Tests") {

  before_each() {
    memset(&test_arena, 0, sizeof(test_arena));
    turbo_arena_init(&test_arena, 2 * 1024 * 1024); // 2MB initial
  }

  after_each() { turbo_arena_free(&test_arena); }

  it("should create a basic buffer") {
    turbo_arena_buffer_t *buffer = turbo_arena_get_buffer(&test_arena, 1024);

    check_not_null(buffer);
    check_not_null(buffer->data);
    check_size_ge(buffer->capacity, 1024);
    check_size_eq(buffer->used, 0);
    check_int_eq(buffer->ref_count, 1);

    turbo_arena_buffer_unref(buffer);
  }

  it("should perform basic allocation") {
    void *ptr = turbo_arena_alloc(&test_arena, 256);
    check_not_null(ptr);

    // Write to the memory to ensure it's valid
    memset(ptr, 0xAA, 256);

    // Verify we can read it back
    unsigned char *data = (unsigned char *)ptr;
    for (int i = 0; i < 256; i++) {
      check_int_eq(0xAA, data[i]);
    }
  }

  it("should get arena statistics") {
    turbo_arena_stats_t stats;
    turbo_arena_get_stats(&test_arena, &stats);

    check_int_ge(stats.region_count, 0);
  }
}