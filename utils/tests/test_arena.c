#include <stdint.h>
#include <string.h>

#include "arena_buffer.h"
#include "tinytest.h"

spec("Arena Tests") {
  describe("Basic Arena Functionality") {
    it("should initialize arena correctly") {
      turbo_arena_t arena;
      int result = turbo_arena_init(&arena, 1024 * 1024);

      check_int_eq(result, 0);
      check_not_null(arena.head);
      check_ptr_eq(arena.head, arena.current);
      check_int_eq(arena.region_count, 1);

      turbo_arena_free(&arena);
    }

    it("should perform basic allocations") {
      turbo_arena_t arena;
      turbo_arena_init(&arena, 1024 * 1024);

      void *ptr1 = turbo_arena_alloc(&arena, 64);
      check_not_null(ptr1);

      void *ptr2 = turbo_arena_alloc(&arena, 128);
      check_not_null(ptr2);
      check_ptr_ne(ptr1, ptr2);

      turbo_arena_free(&arena);
    }

    it("should return NULL for zero-size allocation") {
      turbo_arena_t arena;
      turbo_arena_init(&arena, 1024 * 1024);

      void *ptr = turbo_arena_alloc(&arena, 0);
      check_null(ptr); /* Zero-size allocation should return NULL */

      turbo_arena_free(&arena);
    }

    it("should handle large allocations") {
      turbo_arena_t arena;
      turbo_arena_init(&arena, 1024 * 1024);

      /* Allocate larger than initial region */
      size_t large_size = 2 * 1024 * 1024;
      void *ptr = turbo_arena_alloc(&arena, large_size);
      check_not_null(ptr);

      /* Should be able to allocate more after large allocation */
      void *ptr2 = turbo_arena_alloc(&arena, 64);
      check_not_null(ptr2);

      turbo_arena_free(&arena);
    }
  }

  describe("Memory Alignment") {
    it("should ensure proper alignment for allocations") {
      turbo_arena_t arena;
      turbo_arena_init(&arena, 1024 * 1024);

      /* Test that allocations are properly aligned for the platform */
      void *ptr1 = turbo_arena_alloc(&arena, 1);
      check_not_null(ptr1);

      void *ptr2 = turbo_arena_alloc(&arena, 7);
      check_not_null(ptr2);

      void *ptr3 = turbo_arena_alloc(&arena, 16);
      check_not_null(ptr3);

      /* Verify pointers are at least pointer-aligned */
      check_int_eq((int)((uintptr_t)ptr1 % sizeof(void *)), 0);
      check_int_eq((int)((uintptr_t)ptr2 % sizeof(void *)), 0);
      check_int_eq((int)((uintptr_t)ptr3 % sizeof(void *)), 0);

      /* Verify allocations don't overlap */
      check_ptr_ne(ptr1, ptr2);
      check_ptr_ne(ptr2, ptr3);
      check_ptr_ne(ptr1, ptr3);

      turbo_arena_free(&arena);
    }
  }

  describe("Usage Patterns") {
    it("should maintain data integrity across allocations") {
      turbo_arena_t arena;
      turbo_arena_init(&arena, 1024 * 1024);

      /* Test typical usage patterns */
      char *str1 = (char *)turbo_arena_alloc(&arena, 32);
      check_not_null(str1);
      strcpy(str1, "Hello, Arena!");

      int *numbers = (int *)turbo_arena_alloc(&arena, sizeof(int) * 10);
      check_not_null(numbers);
      for (int i = 0; i < 10; i++) {
        numbers[i] = i * i;
      }

      /* Verify data integrity */
      check_str_eq(str1, "Hello, Arena!");
      for (int i = 0; i < 10; i++) {
        check_int_eq(numbers[i], i * i);
      }

      turbo_arena_free(&arena);
    }
  }
}
