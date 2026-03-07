#include <stdlib.h>
#include <string.h>

#include "turbo_buffer.h"

#include "tinytest.h"

static int free_cb_called = 0;
static void test_free_cb(void *data, void *user_data) {
  (void)data;
  (void)user_data;
  free_cb_called = 1;
}

spec("Arena Buffer Tests") {
  static turbo_pool_t arena;

  before_each() { turbo_pool_init(&arena, 4096); }

  after_each() { turbo_pool_free(&arena); }

  describe("Arena Lifecycle") {
    it("should initialize arena with success") {
      turbo_pool_t test_arena;
      int rc = turbo_pool_init(&test_arena, 1024);
      check_int_eq(rc, 0);
      check_not_null(test_arena.head);
      /* Arena always allocates at least 2MB regions for efficiency */
      check_size_ge(test_arena.head->size, 1024);
      check_int_eq(test_arena.region_count, 1);
      turbo_pool_free(&test_arena);
    }

    it("should initialize with zero size (default)") {
      turbo_pool_t test_arena;
      /* size=0 is valid - uses default region size (2MB) */
      int rc = turbo_pool_init(&test_arena, 0);
      check_int_eq(rc, 0);
      check_not_null(test_arena.head);
      /* Should get default 2MB region */
      check_size_ge(test_arena.head->size, 1024 * 1024);
      turbo_pool_free(&test_arena);
    }

    it("should fail when initializing NULL arena") {
      int rc = turbo_pool_init(NULL, 1024);
      check_int_ne(rc, 0);
    }
  }

  describe("Arena Allocation") {
    it("should perform basic allocation") {
      void *ptr = turbo_pool_alloc(&arena, 128);
      check_not_null(ptr);
    }

    it("should perform multiple allocations") {
      void *ptr1 = turbo_pool_alloc(&arena, 100);
      void *ptr2 = turbo_pool_alloc(&arena, 200);
      void *ptr3 = turbo_pool_alloc(&arena, 300);

      check_not_null(ptr1);
      check_not_null(ptr2);
      check_not_null(ptr3);
      check_ptr_ne(ptr1, ptr2);
      check_ptr_ne(ptr2, ptr3);
    }

    it("should return NULL for zero size allocation") {
      void *ptr = turbo_pool_alloc(&arena, 0);
      check_null(ptr);
    }

    it("should return NULL when allocating from NULL arena") {
      void *ptr = turbo_pool_alloc(NULL, 128);
      check_null(ptr);
    }
  }

  describe("Arena strdup") {
    it("should strdup a string correctly") {
      const char *original = "Hello, TurboNet!";
      char *copy = turbo_pool_strdup(&arena, original);

      check_not_null(copy);
      check_str_eq(copy, original);
      check_ptr_ne(copy, original);
    }

    it("should strdup an empty string correctly") {
      char *copy = turbo_pool_strdup(&arena, "");
      check_not_null(copy);
      check_str_eq(copy, "");
    }

    it("should return NULL when strdup NULL string") {
      char *copy = turbo_pool_strdup(&arena, NULL);
      check_null(copy);
    }
  }

  describe("Buffer Management") {
    it("should get a buffer with basic properties") {
      turbo_pool_buffer_t *buf = turbo_pool_get_buffer(&arena, 256);

      check_not_null(buf);
      check_not_null(buf->data);
      check_size_ge(buf->capacity, 256);
      check_size_eq(buf->used, 0);
      check_int_eq(buf->ref_count, 1);

      turbo_pool_unref(buf);
    }

    it("should correctly handle ref and unref") {
      turbo_pool_buffer_t *buf = turbo_pool_get_buffer(&arena, 128);
      check_int_eq(buf->ref_count, 1);

      turbo_pool_ref(buf);
      check_int_eq(buf->ref_count, 2);

      turbo_pool_ref(buf);
      check_int_eq(buf->ref_count, 3);

      turbo_pool_unref(buf);
      check_int_eq(buf->ref_count, 2);

      turbo_pool_unref(buf);
      turbo_pool_unref(buf);
    }

    it("should set used bytes correctly") {
      turbo_pool_buffer_t *buf = turbo_pool_get_buffer(&arena, 256);

      turbo_pool_set_used(buf, 100);
      check_size_eq(buf->used, 100);

      turbo_pool_set_used(buf, 256);
      check_size_eq(buf->used, 256);

      /* Should not exceed capacity */
      turbo_pool_set_used(buf, 1000);
      check_size_eq(buf->used, 256);

      turbo_pool_unref(buf);
    }

    it("should calculate remaining space correctly") {
      turbo_pool_buffer_t *buf = turbo_pool_get_buffer(&arena, 256);

      check_size_eq(turbo_pool_remaining(buf), 256);

      turbo_pool_set_used(buf, 100);
      check_size_eq(turbo_pool_remaining(buf), 156);

      turbo_pool_unref(buf);
    }

    it("should return correct write pointer") {
      turbo_pool_buffer_t *buf = turbo_pool_get_buffer(&arena, 256);

      char *write_ptr = turbo_pool_write_ptr(buf);
      check_ptr_eq(write_ptr, buf->data);

      turbo_pool_set_used(buf, 50);
      write_ptr = turbo_pool_write_ptr(buf);
      check_ptr_eq(write_ptr, buf->data + 50);

      turbo_pool_unref(buf);
    }
  }

  describe("Slice Operations") {
    it("should create a basic slice") {
      turbo_pool_buffer_t *buf = turbo_pool_get_buffer(&arena, 256);
      memcpy(buf->data, "Hello, World!", 13);
      turbo_pool_set_used(buf, 13);

      turbo_pool_slice_t slice = turbo_pool_slice(buf, 0, 5);
      check_ptr_eq(slice.data, buf->data);
      check_size_eq(slice.length, 5);
      check_ptr_eq(slice.buffer, buf);
      check_int_eq(buf->ref_count, 2);

      turbo_pool_slice_release(&slice);
      check_int_eq(buf->ref_count, 1);

      turbo_pool_unref(buf);
    }

    it("should create a slice with offset") {
      turbo_pool_buffer_t *buf = turbo_pool_get_buffer(&arena, 256);
      memcpy(buf->data, "Hello, World!", 13);
      turbo_pool_set_used(buf, 13);

      turbo_pool_slice_t slice = turbo_pool_slice(buf, 7, 5);
      check_ptr_eq(slice.data, buf->data + 7);
      check_size_eq(slice.length, 5);

      turbo_pool_slice_release(&slice);
      turbo_pool_unref(buf);
    }
  }

  describe("External Buffer Wrapping") {
    it("should wrap external buffer") {
      char *external_data = (char *)malloc(128);
      strcpy(external_data, "External data");

      turbo_pool_buffer_t *buf =
          turbo_pool_wrap_external(external_data, 128, NULL, NULL);

      check_not_null(buf);
      check_ptr_eq(buf->data, external_data);
      check_size_eq(buf->capacity, 128);
      check_int_eq(buf->ref_count, 1);
      check(turbo_pool_is_external(buf));

      turbo_pool_unref(buf);
      free(external_data);
    }

    it("should call free callback when unrefing external buffer") {
      char *external_data = (char *)malloc(64);
      free_cb_called = 0;

      turbo_pool_buffer_t *buf =
          turbo_pool_wrap_external(external_data, 64, test_free_cb, NULL);
      check_not_null(buf);
      check_int_eq(free_cb_called, 0);

      turbo_pool_unref(buf);
      check_int_eq(free_cb_called, 1);

      free(external_data);
    }
  }

  describe("Arena Reset") {
    it("should reset arena correctly") {
      turbo_pool_alloc(&arena, 100);
      turbo_pool_alloc(&arena, 200);

      size_t used_before = arena.total_used;
      check_size_gt(used_before, 0);

      turbo_pool_reset(&arena);
      check_size_eq(arena.total_used, 0);
    }

    it("should get arena usage") {
      turbo_pool_alloc(&arena, 512);

      check_size_ge(arena.total_allocated, 512);
      check_int_ge(arena.region_count, 1);
    }
  }

  describe("Buffer Recycling") {
    it("should recycle buffers correctly") {
      turbo_pool_buffer_t *buf = turbo_pool_get_buffer(&arena, 128);
      check_not_null(buf);
      check_int_eq(arena.recycle_count, 0); // Should be empty initially

      turbo_pool_unref(buf);
      check_int_eq(arena.recycle_count, 1);
      check_ptr_eq(arena.recycle_head, buf);

      turbo_pool_buffer_t *buf2 = turbo_pool_get_buffer(&arena, 128);
      check_ptr_eq(buf2, buf); // Should get the same buffer back
      check_int_eq(arena.recycle_count, 0);

      turbo_pool_unref(buf2);
    }
  }
}
