/**
 * @file turbo_atomic.h
 * @brief Cross-platform atomic operations for thread-safe programming
 *
 * Provides minimal atomic primitives for:
 * - Windows (MSVC, MinGW)
 * - GCC/Clang (Linux, macOS)
 * - C11 stdatomic.h (if available)
 */

#ifndef TURBO_ATOMIC_H
#define TURBO_ATOMIC_H

#include <platform.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Atomic Types
 * ============================================================================ */

#ifdef TURBO_WIN32
typedef volatile LONG turbo_atomic_int_t;
typedef volatile LONG64 turbo_atomic_int64_t;
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  #include <stdatomic.h>
typedef _Atomic int turbo_atomic_int_t;
typedef _Atomic int64_t turbo_atomic_int64_t;
#else
/* GCC/Clang: Use volatile with __sync_* intrinsics */
typedef volatile int turbo_atomic_int_t;
typedef volatile int64_t turbo_atomic_int64_t;
#endif

/* ============================================================================
 * Atomic Int Operations
 * ============================================================================ */

/**
 * @brief Atomically increment and return the NEW value
 */
static inline int turbo_atomic_inc(turbo_atomic_int_t *ptr) {
#ifdef TURBO_WIN32
  return (int)InterlockedIncrement(ptr);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_add(ptr, 1) + 1;
#else
  return __sync_add_and_fetch(ptr, 1);
#endif
}

/**
 * @brief Atomically decrement and return the NEW value
 */
static inline int turbo_atomic_dec(turbo_atomic_int_t *ptr) {
#ifdef TURBO_WIN32
  return (int)InterlockedDecrement(ptr);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_sub(ptr, 1) - 1;
#else
  return __sync_sub_and_fetch(ptr, 1);
#endif
}

/**
 * @brief Atomically load value
 */
static inline int turbo_atomic_load(turbo_atomic_int_t *ptr) {
#ifdef TURBO_WIN32
  return (int)InterlockedCompareExchange(ptr, 0, 0);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_load(ptr);
#else
  __sync_synchronize();
  return *ptr;
#endif
}

/**
 * @brief Atomically store value
 */
static inline void turbo_atomic_store(turbo_atomic_int_t *ptr, int value) {
#ifdef TURBO_WIN32
  InterlockedExchange(ptr, (LONG)value);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  atomic_store(ptr, value);
#else
  __sync_lock_test_and_set(ptr, value);
#endif
}

/**
 * @brief Atomic compare-and-swap
 * @return 1 if swap succeeded, 0 otherwise
 */
static inline int turbo_atomic_cas(turbo_atomic_int_t *ptr, int expected, int desired) {
#ifdef TURBO_WIN32
  return InterlockedCompareExchange(ptr, (LONG)desired, (LONG)expected) == (LONG)expected;
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_compare_exchange_strong(ptr, &expected, desired);
#else
  return __sync_bool_compare_and_swap(ptr, expected, desired);
#endif
}

/* ============================================================================
 * Atomic Int64 Operations
 * ============================================================================ */

/**
 * @brief Atomically load 64-bit value
 */
static inline int64_t turbo_atomic_load64(turbo_atomic_int64_t *ptr) {
#ifdef TURBO_WIN32
  return (int64_t)InterlockedCompareExchange64(ptr, 0, 0);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_load(ptr);
#else
  __sync_synchronize();
  return *ptr;
#endif
}

/**
 * @brief Atomically store 64-bit value
 */
static inline void turbo_atomic_store64(turbo_atomic_int64_t *ptr, int64_t value) {
#ifdef TURBO_WIN32
  InterlockedExchange64(ptr, (LONG64)value);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  atomic_store(ptr, value);
#else
  __sync_lock_test_and_set(ptr, value);
#endif
}

/* ============================================================================
 * Atomic Uint16 Operations (for packet IDs, etc.)
 * ============================================================================ */

#ifdef TURBO_WIN32
typedef volatile SHORT turbo_atomic_uint16_t;
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
typedef _Atomic uint16_t turbo_atomic_uint16_t;
#else
typedef volatile uint16_t turbo_atomic_uint16_t;
#endif

/**
 * @brief Atomically fetch and add to 16-bit value, return OLD value
 */
static inline uint16_t turbo_atomic_fetch_add_uint16(turbo_atomic_uint16_t *ptr, uint16_t value) {
#ifdef TURBO_WIN32
  return (uint16_t)_InterlockedExchangeAdd16(ptr, (SHORT)value);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_add(ptr, value);
#else
  return (uint16_t)__sync_fetch_and_add(ptr, value);
#endif
}

/**
 * @brief Atomically store 16-bit value
 */
static inline void turbo_atomic_store_uint16(turbo_atomic_uint16_t *ptr, uint16_t value) {
#ifdef TURBO_WIN32
  _InterlockedExchange16(ptr, (SHORT)value);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  atomic_store(ptr, value);
#else
  __sync_lock_test_and_set(ptr, value);
#endif
}

/**
 * @brief Atomically load 16-bit value
 */
static inline uint16_t turbo_atomic_load_uint16(turbo_atomic_uint16_t *ptr) {
#ifdef TURBO_WIN32
  return (uint16_t)_InterlockedOr16(ptr, 0);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_load(ptr);
#else
  __sync_synchronize();
  return *ptr;
#endif
}

/* ============================================================================
 * Initialization Macros
 * ============================================================================ */

#define TURBO_ATOMIC_INIT(value) (value)

#ifdef __cplusplus
}
#endif

#endif /* TURBO_ATOMIC_H */
