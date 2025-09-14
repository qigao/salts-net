#include <stdlib.h>
#include <string.h>

#include "arena_buffer.h"
#include "unity.h"

static turbo_arena_t arena;

void setUp(void) { turbo_arena_init(&arena, 4096); }

void tearDown(void) { turbo_arena_free(&arena); }

/* Arena initialization tests */
void test_arena_init_success(void) {
  turbo_arena_t test_arena;
  int rc = turbo_arena_init(&test_arena, 1024);
  TEST_ASSERT_EQUAL(0, rc);
  TEST_ASSERT_NOT_NULL(test_arena.head);
  /* Arena always allocates at least 2MB regions for efficiency */
  TEST_ASSERT_GREATER_OR_EQUAL(1024, test_arena.head->size);
  TEST_ASSERT_EQUAL(1, test_arena.region_count);
  turbo_arena_free(&test_arena);
}

void test_arena_init_zero_size(void) {
  turbo_arena_t test_arena;
  /* size=0 is valid - uses default region size (2MB) */
  int rc = turbo_arena_init(&test_arena, 0);
  TEST_ASSERT_EQUAL(0, rc);
  TEST_ASSERT_NOT_NULL(test_arena.head);
  /* Should get default 2MB region */
  TEST_ASSERT_GREATER_OR_EQUAL(1024 * 1024, test_arena.head->size);
  turbo_arena_free(&test_arena);
}

void test_arena_init_null_arena(void) {
  int rc = turbo_arena_init(NULL, 1024);
  TEST_ASSERT_NOT_EQUAL(0, rc);
}

/* Arena allocation tests */
void test_arena_alloc_basic(void) {
  void *ptr = turbo_arena_alloc(&arena, 128);
  TEST_ASSERT_NOT_NULL(ptr);
}

void test_arena_alloc_multiple(void) {
  void *ptr1 = turbo_arena_alloc(&arena, 100);
  void *ptr2 = turbo_arena_alloc(&arena, 200);
  void *ptr3 = turbo_arena_alloc(&arena, 300);

  TEST_ASSERT_NOT_NULL(ptr1);
  TEST_ASSERT_NOT_NULL(ptr2);
  TEST_ASSERT_NOT_NULL(ptr3);
  TEST_ASSERT_NOT_EQUAL(ptr1, ptr2);
  TEST_ASSERT_NOT_EQUAL(ptr2, ptr3);
}

void test_arena_alloc_zero_size(void) {
  void *ptr = turbo_arena_alloc(&arena, 0);
  TEST_ASSERT_NULL(ptr);
}

void test_arena_alloc_null_arena(void) {
  void *ptr = turbo_arena_alloc(NULL, 128);
  TEST_ASSERT_NULL(ptr);
}

/* Arena strdup tests */
void test_arena_strdup_basic(void) {
  const char *original = "Hello, TurboNet!";
  char *copy = turbo_arena_strdup(&arena, original);

  TEST_ASSERT_NOT_NULL(copy);
  TEST_ASSERT_EQUAL_STRING(original, copy);
  TEST_ASSERT_NOT_EQUAL(original, copy);
}

void test_arena_strdup_empty(void) {
  char *copy = turbo_arena_strdup(&arena, "");
  TEST_ASSERT_NOT_NULL(copy);
  TEST_ASSERT_EQUAL_STRING("", copy);
}

void test_arena_strdup_null(void) {
  char *copy = turbo_arena_strdup(&arena, NULL);
  TEST_ASSERT_NULL(copy);
}

/* Buffer management tests */
void test_arena_get_buffer_basic(void) {
  turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, 256);

  TEST_ASSERT_NOT_NULL(buf);
  TEST_ASSERT_NOT_NULL(buf->data);
  TEST_ASSERT_GREATER_OR_EQUAL(256, buf->capacity);
  TEST_ASSERT_EQUAL(0, buf->used);
  TEST_ASSERT_EQUAL(1, buf->ref_count);

  turbo_arena_buffer_unref(buf);
}

void test_arena_buffer_ref_unref(void) {
  turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, 128);
  TEST_ASSERT_EQUAL(1, buf->ref_count);

  turbo_arena_buffer_ref(buf);
  TEST_ASSERT_EQUAL(2, buf->ref_count);

  turbo_arena_buffer_ref(buf);
  TEST_ASSERT_EQUAL(3, buf->ref_count);

  turbo_arena_buffer_unref(buf);
  TEST_ASSERT_EQUAL(2, buf->ref_count);

  turbo_arena_buffer_unref(buf);
  turbo_arena_buffer_unref(buf);
}

void test_arena_buffer_set_used(void) {
  turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, 256);

  turbo_arena_buffer_set_used(buf, 100);
  TEST_ASSERT_EQUAL(100, buf->used);

  turbo_arena_buffer_set_used(buf, 256);
  TEST_ASSERT_EQUAL(256, buf->used);

  /* Should not exceed capacity */
  turbo_arena_buffer_set_used(buf, 1000);
  TEST_ASSERT_EQUAL(256, buf->used);

  turbo_arena_buffer_unref(buf);
}

void test_arena_buffer_remaining(void) {
  turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, 256);

  TEST_ASSERT_EQUAL(256, turbo_arena_buffer_remaining(buf));

  turbo_arena_buffer_set_used(buf, 100);
  TEST_ASSERT_EQUAL(156, turbo_arena_buffer_remaining(buf));

  turbo_arena_buffer_unref(buf);
}

void test_arena_buffer_write_ptr(void) {
  turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, 256);

  char *write_ptr = turbo_arena_buffer_write_ptr(buf);
  TEST_ASSERT_EQUAL(buf->data, write_ptr);

  turbo_arena_buffer_set_used(buf, 50);
  write_ptr = turbo_arena_buffer_write_ptr(buf);
  TEST_ASSERT_EQUAL(buf->data + 50, write_ptr);

  turbo_arena_buffer_unref(buf);
}

/* Slice tests */
void test_arena_buffer_slice_basic(void) {
  turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, 256);
  memcpy(buf->data, "Hello, World!", 13);
  turbo_arena_buffer_set_used(buf, 13);

  turbo_arena_slice_t slice = turbo_arena_buffer_slice(buf, 0, 5);
  TEST_ASSERT_EQUAL(buf->data, slice.data);
  TEST_ASSERT_EQUAL(5, slice.length);
  TEST_ASSERT_EQUAL(buf, slice.buffer);
  TEST_ASSERT_EQUAL(2, buf->ref_count);

  turbo_arena_slice_release(&slice);
  TEST_ASSERT_EQUAL(1, buf->ref_count);

  turbo_arena_buffer_unref(buf);
}

void test_arena_buffer_slice_offset(void) {
  turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, 256);
  memcpy(buf->data, "Hello, World!", 13);
  turbo_arena_buffer_set_used(buf, 13);

  turbo_arena_slice_t slice = turbo_arena_buffer_slice(buf, 7, 5);
  TEST_ASSERT_EQUAL(buf->data + 7, slice.data);
  TEST_ASSERT_EQUAL(5, slice.length);

  turbo_arena_slice_release(&slice);
  turbo_arena_buffer_unref(buf);
}

/* External buffer wrapping tests */
void test_arena_wrap_external_basic(void) {
  char *external_data = (char *)malloc(128);
  strcpy(external_data, "External data");

  turbo_arena_buffer_t *buf = turbo_arena_wrap_external(external_data, 128, NULL, NULL);

  TEST_ASSERT_NOT_NULL(buf);
  TEST_ASSERT_EQUAL(external_data, buf->data);
  TEST_ASSERT_EQUAL(128, buf->capacity);
  TEST_ASSERT_EQUAL(1, buf->ref_count);
  TEST_ASSERT_EQUAL(1, turbo_arena_buffer_is_external(buf));

  turbo_arena_buffer_unref(buf);
  free(external_data);
}

static int free_cb_called = 0;
static void test_free_cb(void *data, void *user_data) {
  (void)data;
  (void)user_data;
  free_cb_called = 1;
}

void test_arena_wrap_external_with_callback(void) {
  char *external_data = (char *)malloc(64);
  free_cb_called = 0;

  turbo_arena_buffer_t *buf = turbo_arena_wrap_external(external_data, 64, test_free_cb, NULL);
  TEST_ASSERT_NOT_NULL(buf);
  TEST_ASSERT_EQUAL(0, free_cb_called);

  turbo_arena_buffer_unref(buf);
  TEST_ASSERT_EQUAL(1, free_cb_called);

  free(external_data);
}

/* Arena reset tests */
void test_arena_reset(void) {
  turbo_arena_alloc(&arena, 100);
  turbo_arena_alloc(&arena, 200);

  size_t used_before = arena.total_used;
  TEST_ASSERT_GREATER_THAN(0, used_before);

  turbo_arena_reset(&arena);
  TEST_ASSERT_EQUAL(0, arena.total_used);
}

/* Statistics tests */
void test_arena_get_stats(void) {
  turbo_arena_stats_t stats;
  turbo_arena_alloc(&arena, 512);

  turbo_arena_get_stats(&arena, &stats);
  TEST_ASSERT_GREATER_OR_EQUAL(512, stats.total_allocated);
  TEST_ASSERT_GREATER_OR_EQUAL(1, stats.region_count);
}

int main(void) {
  UNITY_BEGIN();

  /* Arena lifecycle */
  RUN_TEST(test_arena_init_success);
  RUN_TEST(test_arena_init_zero_size);
  RUN_TEST(test_arena_init_null_arena);

  /* Arena allocation */
  RUN_TEST(test_arena_alloc_basic);
  RUN_TEST(test_arena_alloc_multiple);
  RUN_TEST(test_arena_alloc_zero_size);
  RUN_TEST(test_arena_alloc_null_arena);

  /* Arena strdup */
  RUN_TEST(test_arena_strdup_basic);
  RUN_TEST(test_arena_strdup_empty);
  RUN_TEST(test_arena_strdup_null);

  /* Buffer management */
  RUN_TEST(test_arena_get_buffer_basic);
  RUN_TEST(test_arena_buffer_ref_unref);
  RUN_TEST(test_arena_buffer_set_used);
  RUN_TEST(test_arena_buffer_remaining);
  RUN_TEST(test_arena_buffer_write_ptr);

  /* Slice operations */
  RUN_TEST(test_arena_buffer_slice_basic);
  RUN_TEST(test_arena_buffer_slice_offset);

  /* External buffer wrapping */
  RUN_TEST(test_arena_wrap_external_basic);
  RUN_TEST(test_arena_wrap_external_with_callback);

  /* Arena reset and stats */
  RUN_TEST(test_arena_reset);
  RUN_TEST(test_arena_get_stats);

  return UNITY_END();
}
