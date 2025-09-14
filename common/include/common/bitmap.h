/**
 * @file bitmap.h
 * @brief Common bitmap utilities for efficient bit manipulation
 *
 * Provides fast bitmap operations using 64-bit words for better performance
 * than byte-based approaches. Used by stats system and other components.
 */

#ifndef TURBO_BITMAP_H
#define TURBO_BITMAP_H

#include <platform.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Calculate number of 64-bit words needed for a bitmap of given capacity
 * @param capacity Number of bits needed
 * @return Number of uint64_t words required
 */
static inline size_t turbo_bitmap_words(size_t capacity) { return (capacity + 63) / 64; }

/**
 * @brief Set a bit in the bitmap
 * @param bitmap Pointer to bitmap array
 * @param index Bit index to set
 */
static inline void turbo_bitmap_set(uint64_t *bitmap, size_t index) {
  bitmap[index / 64] |= (uint64_t)1 << (index % 64);
}

/**
 * @brief Clear a bit in the bitmap
 * @param bitmap Pointer to bitmap array
 * @param index Bit index to clear
 */
static inline void turbo_bitmap_clear(uint64_t *bitmap, size_t index) {
  bitmap[index / 64] &= ~((uint64_t)1 << (index % 64));
}

/**
 * @brief Test if a bit is set in the bitmap
 * @param bitmap Pointer to bitmap array
 * @param index Bit index to test
 * @return true if bit is set, false otherwise
 */
static inline bool turbo_bitmap_test(const uint64_t *bitmap, size_t index) {
  return (bitmap[index / 64] >> (index % 64)) & 1U;
}

/**
 * @brief Clear all bits in the bitmap
 * @param bitmap Pointer to bitmap array
 * @param word_count Number of 64-bit words in the bitmap
 */
CXX_C_API void turbo_bitmap_clear_all(uint64_t *bitmap, size_t word_count);

/**
 * @brief Set all bits in the bitmap
 * @param bitmap Pointer to bitmap array
 * @param word_count Number of 64-bit words in the bitmap
 */
CXX_C_API void turbo_bitmap_set_all(uint64_t *bitmap, size_t word_count);

/**
 * @brief Find first unset bit in the bitmap
 * @param bitmap Pointer to bitmap array
 * @param capacity Total number of bits in the bitmap
 * @return Index of first unset bit, or SIZE_MAX if all bits are set
 */
CXX_C_API size_t turbo_bitmap_find_first_unset(const uint64_t *bitmap, size_t capacity);

/**
 * @brief Count number of set bits in the bitmap
 * @param bitmap Pointer to bitmap array
 * @param capacity Total number of bits in the bitmap
 * @return Number of set bits
 */
CXX_C_API size_t turbo_bitmap_popcount(const uint64_t *bitmap, size_t capacity);

/**
 * @brief Find first set bit in the bitmap
 * @param bitmap Pointer to bitmap array
 * @param capacity Total number of bits in the bitmap
 * @return Index of first set bit, or SIZE_MAX if no bits are set
 */
CXX_C_API size_t turbo_bitmap_find_first_set(const uint64_t *bitmap, size_t capacity);

/**
 * @brief Find last set bit in the bitmap
 * @param bitmap Pointer to bitmap array
 * @param capacity Total number of bits in the bitmap
 * @return Index of last set bit, or SIZE_MAX if no bits are set
 */
CXX_C_API size_t turbo_bitmap_find_last_set(const uint64_t *bitmap, size_t capacity);

/**
 * @brief Check if all bits in the bitmap are set
 * @param bitmap Pointer to bitmap array
 * @param capacity Total number of bits in the bitmap
 * @return true if all bits are set, false otherwise
 */
static inline bool turbo_bitmap_is_full(const uint64_t *bitmap, size_t capacity) {
  return turbo_bitmap_popcount(bitmap, capacity) == capacity;
}

/**
 * @brief Check if all bits in the bitmap are clear
 * @param bitmap Pointer to bitmap array
 * @param capacity Total number of bits in the bitmap
 * @return true if all bits are clear, false otherwise
 */
static inline bool turbo_bitmap_is_empty(const uint64_t *bitmap, size_t capacity) {
  return turbo_bitmap_popcount(bitmap, capacity) == 0;
}

/**
 * @brief Toggle a bit in the bitmap
 * @param bitmap Pointer to bitmap array
 * @param index Bit index to toggle
 */
static inline void turbo_bitmap_toggle(uint64_t *bitmap, size_t index) {
  bitmap[index / 64] ^= (uint64_t)1 << (index % 64);
}

/**
 * @brief Set a range of bits in the bitmap
 * @param bitmap Pointer to bitmap array
 * @param start_index Starting bit index (inclusive)
 * @param end_index Ending bit index (exclusive)
 */
CXX_C_API void turbo_bitmap_set_range(uint64_t *bitmap, size_t start_index, size_t end_index);

/**
 * @brief Clear a range of bits in the bitmap
 * @param bitmap Pointer to bitmap array
 * @param start_index Starting bit index (inclusive)
 * @param end_index Ending bit index (exclusive)
 */
CXX_C_API void turbo_bitmap_clear_range(uint64_t *bitmap, size_t start_index, size_t end_index);

/**
 * @brief Perform bitwise AND operation between two bitmaps
 * @param dest Destination bitmap (result stored here)
 * @param src1 First source bitmap
 * @param src2 Second source bitmap
 * @param word_count Number of 64-bit words in the bitmaps
 */
static inline void turbo_bitmap_and(uint64_t *dest, const uint64_t *src1, const uint64_t *src2,
                                    size_t word_count) {
  for (size_t i = 0; i < word_count; i++) {
    dest[i] = src1[i] & src2[i];
  }
}

/**
 * @brief Perform bitwise OR operation between two bitmaps
 * @param dest Destination bitmap (result stored here)
 * @param src1 First source bitmap
 * @param src2 Second source bitmap
 * @param word_count Number of 64-bit words in the bitmaps
 */
static inline void turbo_bitmap_or(uint64_t *dest, const uint64_t *src1, const uint64_t *src2,
                                   size_t word_count) {
  for (size_t i = 0; i < word_count; i++) {
    dest[i] = src1[i] | src2[i];
  }
}

/**
 * @brief Perform bitwise XOR operation between two bitmaps
 * @param dest Destination bitmap (result stored here)
 * @param src1 First source bitmap
 * @param src2 Second source bitmap
 * @param word_count Number of 64-bit words in the bitmaps
 */
static inline void turbo_bitmap_xor(uint64_t *dest, const uint64_t *src1, const uint64_t *src2,
                                    size_t word_count) {
  for (size_t i = 0; i < word_count; i++) {
    dest[i] = src1[i] ^ src2[i];
  }
}

/**
 * @brief Copy one bitmap to another
 * @param dest Destination bitmap
 * @param src Source bitmap
 * @param word_count Number of 64-bit words to copy
 */
static inline void turbo_bitmap_copy(uint64_t *dest, const uint64_t *src, size_t word_count) {
  memcpy(dest, src, word_count * sizeof(uint64_t));
}

/**
 * @brief Simple hash function for string keys (FNV-1a)
 * @param str String to hash
 * @return 32-bit hash value
 */
static inline uint32_t turbo_bitmap_hash_string(const char *str) {
  uint32_t hash = 2166136261u;
  while (*str) {
    hash ^= (uint8_t)*str++;
    hash *= 16777619u;
  }
  return hash;
}

/**
 * @brief Hash a string to a slot index within capacity
 * @param str String to hash
 * @param capacity Maximum slot index + 1
 * @return Slot index in range [0, capacity)
 */
static inline uint32_t turbo_bitmap_hash_to_slot(const char *str, uint32_t capacity) {
  return turbo_bitmap_hash_string(str) % capacity;
}

#ifdef __cplusplus
}
#endif

#endif /* TURBO_BITMAP_H */