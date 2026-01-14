#include <string.h>
#include <stdint.h>

#include "unity.h"
#include "arena_buffer.h"

static turbo_arena_t test_arena;

void setUp(void)
{
    memset(&test_arena, 0, sizeof(test_arena));
    turbo_arena_init(&test_arena, 2 * 1024 * 1024); // 2MB initial
}

void tearDown(void)
{
    turbo_arena_free(&test_arena);
}

/* Simple test to verify basic functionality */
void test_simple_buffer_creation(void)
{
    turbo_arena_buffer_t* buffer = turbo_arena_get_buffer(&test_arena, 1024);
    
    if (buffer) {
        TEST_ASSERT_NOT_NULL(buffer);
        TEST_ASSERT_NOT_NULL(buffer->data);
        TEST_ASSERT_GREATER_OR_EQUAL(1024, buffer->capacity);
        TEST_ASSERT_EQUAL(0, buffer->used);
        TEST_ASSERT_EQUAL(1, buffer->ref_count);
        
        turbo_arena_buffer_unref(buffer);
        TEST_PASS();
    } else {
        TEST_FAIL_MESSAGE("Buffer creation failed");
    }
}

/* Test basic allocation */
void test_simple_allocation(void)
{
    void* ptr = turbo_arena_alloc(&test_arena, 256);
    TEST_ASSERT_NOT_NULL(ptr);
    
    // Write to the memory to ensure it's valid
    memset(ptr, 0xAA, 256);
    
    // Verify we can read it back
    unsigned char* data = (unsigned char*)ptr;
    for (int i = 0; i < 256; i++) {
        TEST_ASSERT_EQUAL(0xAA, data[i]);
    }
}

/* Test statistics function exists */
void test_simple_statistics(void)
{
    turbo_arena_stats_t stats;
    turbo_arena_get_stats(&test_arena, &stats);
    
    // Just verify the function doesn't crash
    TEST_ASSERT_GREATER_OR_EQUAL(0, stats.region_count);
}

int main(void)
{
    UNITY_BEGIN();
    
    RUN_TEST(test_simple_allocation);
    RUN_TEST(test_simple_buffer_creation);
    RUN_TEST(test_simple_statistics);
    
    return UNITY_END();
}