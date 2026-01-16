/**
 * @file bitmap.c
 * @brief Common bitmap utilities implementation
 */

#include "bitmap.h"
#include <string.h>

void turbo_bitmap_clear_all(uint64_t *bitmap, size_t word_count) {
    if (!bitmap) return;
    memset(bitmap, 0, word_count * sizeof(uint64_t));
}

void turbo_bitmap_set_all(uint64_t *bitmap, size_t word_count) {
    if (!bitmap) return;
    for (size_t i = 0; i < word_count; i++) {
        bitmap[i] = UINT64_MAX;
    }
}

size_t turbo_bitmap_find_first_unset(const uint64_t *bitmap, size_t capacity) {
    if (!bitmap) return SIZE_MAX;
    
    size_t word_count = turbo_bitmap_words(capacity);
    
    for (size_t word_idx = 0; word_idx < word_count; word_idx++) {
        uint64_t word = bitmap[word_idx];
        
        // If word is not all 1s, there's at least one unset bit
        if (word != UINT64_MAX) {
            // Find first unset bit in this word
            for (size_t bit_idx = 0; bit_idx < 64; bit_idx++) {
                size_t global_idx = word_idx * 64 + bit_idx;
                if (global_idx >= capacity) {
                    return SIZE_MAX; // Past capacity
                }
                if ((word & ((uint64_t)1 << bit_idx)) == 0) {
                    return global_idx;
                }
            }
        }
    }
    
    return SIZE_MAX; // All bits are set
}

size_t turbo_bitmap_popcount(const uint64_t *bitmap, size_t capacity) {
    if (!bitmap) return 0;
    
    size_t word_count = turbo_bitmap_words(capacity);
    size_t count = 0;
    
    for (size_t word_idx = 0; word_idx < word_count; word_idx++) {
        uint64_t word = bitmap[word_idx];
        
        // Handle partial last word
        if (word_idx == word_count - 1) {
            size_t remaining_bits = capacity % 64;
            if (remaining_bits > 0) {
                // Mask off bits beyond capacity
                uint64_t mask = ((uint64_t)1 << remaining_bits) - 1;
                word &= mask;
            }
        }
        
        // Use builtin popcount for efficiency
        #if defined(__GNUC__) || defined(__clang__)
            count += __builtin_popcountll(word);
        #elif defined(_MSC_VER)
            count += __popcnt64(word);
        #else
            // Fallback implementation
            while (word) {
                count += word & 1;
                word >>= 1;
            }
        #endif
    }
    
    return count;
}
size_t turbo_bitmap_find_first_set(const uint64_t *bitmap, size_t capacity) {
    if (!bitmap) return SIZE_MAX;
    
    size_t word_count = turbo_bitmap_words(capacity);
    
    for (size_t word_idx = 0; word_idx < word_count; word_idx++) {
        uint64_t word = bitmap[word_idx];
        
        if (word != 0) {
            // Find first set bit in this word
            #if defined(__GNUC__) || defined(__clang__)
                int bit_idx = __builtin_ctzll(word);
            #elif defined(_MSC_VER)
                unsigned long bit_idx;
                _BitScanForward64(&bit_idx, word);
            #else
                // Fallback implementation
                int bit_idx = 0;
                while ((word & 1) == 0) {
                    word >>= 1;
                    bit_idx++;
                }
            #endif
            
            size_t global_idx = word_idx * 64 + bit_idx;
            if (global_idx < capacity) {
                return global_idx;
            }
        }
    }
    
    return SIZE_MAX; // No bits are set
}

size_t turbo_bitmap_find_last_set(const uint64_t *bitmap, size_t capacity) {
    if (!bitmap || capacity == 0) return SIZE_MAX;
    
    size_t word_count = turbo_bitmap_words(capacity);
    
    // Start from the last word and work backwards
    for (size_t word_idx = word_count; word_idx > 0; word_idx--) {
        size_t idx = word_idx - 1;
        uint64_t word = bitmap[idx];
        
        // Handle partial last word
        if (idx == word_count - 1) {
            size_t remaining_bits = capacity % 64;
            if (remaining_bits > 0) {
                // Mask off bits beyond capacity
                uint64_t mask = ((uint64_t)1 << remaining_bits) - 1;
                word &= mask;
            }
        }
        
        if (word != 0) {
            // Find last set bit in this word
            #if defined(__GNUC__) || defined(__clang__)
                int bit_idx = 63 - __builtin_clzll(word);
            #elif defined(_MSC_VER)
                unsigned long bit_idx;
                _BitScanReverse64(&bit_idx, word);
            #else
                // Fallback implementation
                int bit_idx = 63;
                while ((word & ((uint64_t)1 << bit_idx)) == 0) {
                    bit_idx--;
                }
            #endif
            
            return idx * 64 + bit_idx;
        }
    }
    
    return SIZE_MAX; // No bits are set
}

void turbo_bitmap_set_range(uint64_t *bitmap, size_t start_index, size_t end_index) {
    if (!bitmap || start_index >= end_index) return;
    
    for (size_t i = start_index; i < end_index; i++) {
        turbo_bitmap_set(bitmap, i);
    }
}

void turbo_bitmap_clear_range(uint64_t *bitmap, size_t start_index, size_t end_index) {
    if (!bitmap || start_index >= end_index) return;
    
    for (size_t i = start_index; i < end_index; i++) {
        turbo_bitmap_clear(bitmap, i);
    }
}
