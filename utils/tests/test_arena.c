#include <stdint.h>
#include <string.h>


#include "arena_buffer.h"
#include "unity.h"


void setUp(void) { /* Set up before each test */ }

void tearDown(void) { /* Clean up after each test */ }

/* Basic enhanced arena tests */
void test_arena_init(void) {
  turbo_arena_t arena;
  int result = turbo_arena_init(&arena, 1024 * 1024);

  TEST_ASSERT_EQUAL(0, result);
  TEST_ASSERT_NOT_NULL(arena.head);
  TEST_ASSERT_EQUAL(arena.head, arena.current);
  TEST_ASSERT_EQUAL(1, arena.region_count);

  turbo_arena_free(&arena);
}

void test_arena_basic_alloc(void) {
  turbo_arena_t arena;
  turbo_arena_init(&arena, 1024 * 1024);

  void *ptr1 = turbo_arena_alloc(&arena, 64);
  TEST_ASSERT_NOT_NULL(ptr1);

  void *ptr2 = turbo_arena_alloc(&arena, 128);
  TEST_ASSERT_NOT_NULL(ptr2);
  TEST_ASSERT_NOT_EQUAL(ptr1, ptr2);

  turbo_arena_free(&arena);
}

void test_arena_zero_alloc(void) {
  turbo_arena_t arena;
  turbo_arena_init(&arena, 1024 * 1024);

  void *ptr = turbo_arena_alloc(&arena, 0);
  TEST_ASSERT_NULL(ptr); /* Zero-size allocation should return NULL */

  turbo_arena_free(&arena);
}

void test_arena_large_alloc(void) {
  turbo_arena_t arena;
  turbo_arena_init(&arena, 1024 * 1024);

  /* Allocate larger than initial region */
  size_t large_size = 2 * 1024 * 1024;
  void *ptr = turbo_arena_alloc(&arena, large_size);
  TEST_ASSERT_NOT_NULL(ptr);

  /* Should be able to allocate more after large allocation */
  void *ptr2 = turbo_arena_alloc(&arena, 64);
  TEST_ASSERT_NOT_NULL(ptr2);

  turbo_arena_free(&arena);
}

/* Memory alignment tests */
void test_arena_alignment(void) {
  turbo_arena_t arena;
  turbo_arena_init(&arena, 1024 * 1024);

  /* Test that allocations are properly aligned for the platform */
  /* The actual alignment depends on the region header structure */
  void *ptr1 = turbo_arena_alloc(&arena, 1);
  TEST_ASSERT_NOT_NULL(ptr1);
  
  void *ptr2 = turbo_arena_alloc(&arena, 7);
  TEST_ASSERT_NOT_NULL(ptr2);
  
  void *ptr3 = turbo_arena_alloc(&arena, 16);
  TEST_ASSERT_NOT_NULL(ptr3);
  
  /* Verify pointers are at least pointer-aligned */
  TEST_ASSERT_EQUAL(0, (uintptr_t)ptr1 % sizeof(void*));
  TEST_ASSERT_EQUAL(0, (uintptr_t)ptr2 % sizeof(void*));
  TEST_ASSERT_EQUAL(0, (uintptr_t)ptr3 % sizeof(void*));
  
  /* Verify allocations don't overlap */
  TEST_ASSERT_NOT_EQUAL(ptr1, ptr2);
  TEST_ASSERT_NOT_EQUAL(ptr2, ptr3);
  TEST_ASSERT_NOT_EQUAL(ptr1, ptr3);

  turbo_arena_free(&arena);
}

/* Test arena usage patterns */
void test_arena_usage_patterns(void) {
  turbo_arena_t arena;
  turbo_arena_init(&arena, 1024 * 1024);

  /* Test typical usage patterns */
  char *str1 = (char*)turbo_arena_alloc(&arena, 32);
  TEST_ASSERT_NOT_NULL(str1);
  strcpy(str1, "Hello, Arena!");
  
  int *numbers = (int*)turbo_arena_alloc(&arena, sizeof(int) * 10);
  TEST_ASSERT_NOT_NULL(numbers);
  for (int i = 0; i < 10; i++) {
    numbers[i] = i * i;
  }
  
  /* Verify data integrity */
  TEST_ASSERT_EQUAL_STRING("Hello, Arena!", str1);
  for (int i = 0; i < 10; i++) {
    TEST_ASSERT_EQUAL(i * i, numbers[i]);
  }

  turbo_arena_free(&arena);
}

int main(void) {
  UNITY_BEGIN();

  /* Basic enhanced arena functionality */
  RUN_TEST(test_arena_init);
  RUN_TEST(test_arena_basic_alloc);
  RUN_TEST(test_arena_zero_alloc);
  RUN_TEST(test_arena_large_alloc);
  RUN_TEST(test_arena_alignment);
  RUN_TEST(test_arena_usage_patterns);

  return UNITY_END();
}
