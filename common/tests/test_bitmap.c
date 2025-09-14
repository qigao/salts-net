/**
 * @file test_bitmap.c
 * @brief Unit tests for bitmap utilities
 */

#include "bitmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

void test_basic_operations() {
    printf("Testing basic bitmap operations...\n");
    
    // Test with 128 bits (2 words)
    size_t capacity = 128;
    size_t word_count = turbo_bitmap_words(capacity);
    uint64_t *bitmap = calloc(word_count, sizeof(uint64_t));
    
    assert(bitmap != NULL);
    assert(word_count == 2);
    
    // Initially all bits should be clear
    assert(turbo_bitmap_is_empty(bitmap, capacity));
    assert(!turbo_bitmap_is_full(bitmap, capacity));
    assert(turbo_bitmap_popcount(bitmap, capacity) == 0);
    
    // Set some bits
    turbo_bitmap_set(bitmap, 0);
    turbo_bitmap_set(bitmap, 63);
    turbo_bitmap_set(bitmap, 64);
    turbo_bitmap_set(bitmap, 127);
    
    assert(!turbo_bitmap_is_empty(bitmap, capacity));
    assert(!turbo_bitmap_is_full(bitmap, capacity));
    assert(turbo_bitmap_popcount(bitmap, capacity) == 4);
    
    // Test individual bit checks
    assert(turbo_bitmap_test(bitmap, 0));
    assert(turbo_bitmap_test(bitmap, 63));
    assert(turbo_bitmap_test(bitmap, 64));
    assert(turbo_bitmap_test(bitmap, 127));
    assert(!turbo_bitmap_test(bitmap, 1));
    assert(!turbo_bitmap_test(bitmap, 62));
    
    // Test find operations
    assert(turbo_bitmap_find_first_set(bitmap, capacity) == 0);
    assert(turbo_bitmap_find_last_set(bitmap, capacity) == 127);
    assert(turbo_bitmap_find_first_unset(bitmap, capacity) == 1);
    
    // Clear some bits
    turbo_bitmap_clear(bitmap, 0);
    turbo_bitmap_clear(bitmap, 127);
    
    assert(turbo_bitmap_popcount(bitmap, capacity) == 2);
    assert(turbo_bitmap_find_first_set(bitmap, capacity) == 63);
    assert(turbo_bitmap_find_last_set(bitmap, capacity) == 64);
    assert(turbo_bitmap_find_first_unset(bitmap, capacity) == 0);
    
    free(bitmap);
    printf("✓ Basic operations test passed\n");
}

void test_range_operations() {
    printf("Testing range operations...\n");
    
    size_t capacity = 100;
    size_t word_count = turbo_bitmap_words(capacity);
    uint64_t *bitmap = calloc(word_count, sizeof(uint64_t));
    
    // Set range [10, 20)
    turbo_bitmap_set_range(bitmap, 10, 20);
    
    assert(turbo_bitmap_popcount(bitmap, capacity) == 10);
    
    // Check that bits 10-19 are set
    for (size_t i = 10; i < 20; i++) {
        assert(turbo_bitmap_test(bitmap, i));
    }
    
    // Check that other bits are clear
    for (size_t i = 0; i < 10; i++) {
        assert(!turbo_bitmap_test(bitmap, i));
    }
    for (size_t i = 20; i < capacity; i++) {
        assert(!turbo_bitmap_test(bitmap, i));
    }
    
    // Clear range [15, 25) - should clear 15-19
    turbo_bitmap_clear_range(bitmap, 15, 25);
    
    assert(turbo_bitmap_popcount(bitmap, capacity) == 5);
    
    // Check that bits 10-14 are still set
    for (size_t i = 10; i < 15; i++) {
        assert(turbo_bitmap_test(bitmap, i));
    }
    
    // Check that bits 15-19 are now clear
    for (size_t i = 15; i < 20; i++) {
        assert(!turbo_bitmap_test(bitmap, i));
    }
    
    free(bitmap);
    printf("✓ Range operations test passed\n");
}

void test_bitmap_operations() {
    printf("Testing bitmap set operations...\n");
    
    size_t capacity = 64;
    size_t word_count = turbo_bitmap_words(capacity);
    uint64_t *bitmap1 = calloc(word_count, sizeof(uint64_t));
    uint64_t *bitmap2 = calloc(word_count, sizeof(uint64_t));
    uint64_t *result = calloc(word_count, sizeof(uint64_t));
    
    // Set some bits in bitmap1: 0, 2, 4, 6, 8
    for (size_t i = 0; i < 10; i += 2) {
        turbo_bitmap_set(bitmap1, i);
    }
    
    // Set some bits in bitmap2: 1, 2, 3, 4, 5
    for (size_t i = 1; i < 6; i++) {
        turbo_bitmap_set(bitmap2, i);
    }
    
    // Test AND operation
    turbo_bitmap_and(result, bitmap1, bitmap2, word_count);
    assert(turbo_bitmap_popcount(result, capacity) == 2); // bits 2 and 4
    assert(turbo_bitmap_test(result, 2));
    assert(turbo_bitmap_test(result, 4));
    
    // Test OR operation
    turbo_bitmap_or(result, bitmap1, bitmap2, word_count);
    assert(turbo_bitmap_popcount(result, capacity) == 8); // bits 0,1,2,3,4,5,6,8
    
    // Test XOR operation
    turbo_bitmap_xor(result, bitmap1, bitmap2, word_count);
    assert(turbo_bitmap_popcount(result, capacity) == 6); // bits 0,1,3,5,6,8
    assert(turbo_bitmap_test(result, 0));
    assert(turbo_bitmap_test(result, 1));
    assert(!turbo_bitmap_test(result, 2)); // 2 is in both, so XOR = 0
    assert(turbo_bitmap_test(result, 3));
    assert(!turbo_bitmap_test(result, 4)); // 4 is in both, so XOR = 0
    assert(turbo_bitmap_test(result, 5));
    
    free(bitmap1);
    free(bitmap2);
    free(result);
    printf("✓ Bitmap operations test passed\n");
}

void test_edge_cases() {
    printf("Testing edge cases...\n");
    
    // Test with capacity that's not a multiple of 64
    size_t capacity = 100;
    size_t word_count = turbo_bitmap_words(capacity);
    uint64_t *bitmap = calloc(word_count, sizeof(uint64_t));
    
    assert(word_count == 2); // 100 bits needs 2 words
    
    // Set all bits
    turbo_bitmap_set_all(bitmap, word_count);
    
    // The popcount should only count up to capacity, not full words
    assert(turbo_bitmap_popcount(bitmap, capacity) == capacity);
    
    // Test that bits beyond capacity are properly masked
    turbo_bitmap_set(bitmap, 127); // This should be ignored in popcount
    assert(turbo_bitmap_popcount(bitmap, capacity) == capacity);
    
    // Test find operations with partial last word
    assert(turbo_bitmap_find_last_set(bitmap, capacity) == 99);
    
    free(bitmap);
    printf("✓ Edge cases test passed\n");
}

int main() {
    printf("Running bitmap utility tests...\n\n");
    
    test_basic_operations();
    test_range_operations();
    test_bitmap_operations();
    test_edge_cases();
    
    printf("\n✅ All bitmap tests passed!\n");
    return 0;
}