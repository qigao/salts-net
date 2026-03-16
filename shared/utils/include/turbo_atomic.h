/**
 * @file turbo_atomic.h
 * @brief Cross-platform atomic operations for thread-safe programming
 *
 * Provides minimal atomic primitives for:
 * - Windows (MSVC, MinGW)
 * - GCC/Clang (Linux, macOS)
 * - C11 stdatomic.h (if available)
 */

#ifndef T_ATOMIC_H
#define T_ATOMIC_H

#include <stdbool.h>
#include <stdint.h>

/* Platform detection */
#if defined(_WIN32) || defined(_WIN64)
  #include <windows.h>
  #include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Atomic Types
 * ============================================================================ */

#if defined(__cplusplus)
  /* no C atomic */
#elif defined(__has_include)
  #if __has_include(<stdatomic.h>) && !defined(__STDC_NO_ATOMICS__)
    #define TURBO_HAS_C11_ATOMICS 1
  #endif
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  #define TURBO_HAS_C11_ATOMICS 1
#endif

#if defined(TURBO_HAS_C11_ATOMICS)
  /* C11 atomics */
  #include <stdatomic.h>
  typedef _Atomic int t_atomic_int_t;
  typedef _Atomic int64_t t_atomic_int64_t;
#elif defined(_WIN32) || defined(_WIN64)
  /* Windows: use volatile with Interlocked* */
  typedef volatile LONG t_atomic_int_t;
  typedef volatile LONG64 t_atomic_int64_t;
#else
  /* GCC/Clang: use volatile with __atomic_* */
  typedef volatile int t_atomic_int_t;
  typedef volatile int64_t t_atomic_int64_t;
#endif

/* ============================================================================
 * Atomic Int Operations
 * ============================================================================ */

/**
 * @brief Atomically increment and return the NEW value
 */
static inline int t_atomic_inc(t_atomic_int_t *ptr) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_add(ptr, 1) + 1;
#elif defined(_WIN32) || defined(_WIN64)
  return (int)InterlockedIncrement(ptr);
#else
  return __atomic_add_fetch(ptr, 1, __ATOMIC_SEQ_CST);
#endif
}

/**
 * @brief Atomically decrement and return the NEW value
 */
static inline int t_atomic_dec(t_atomic_int_t *ptr) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_sub(ptr, 1) - 1;
#elif defined(_WIN32) || defined(_WIN64)
  return (int)InterlockedDecrement(ptr);
#else
  return __atomic_sub_fetch(ptr, 1, __ATOMIC_SEQ_CST);
#endif
}

/**
 * @brief Atomically load value (acquire semantics)
 */
static inline int t_atomic_load(t_atomic_int_t *ptr) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_load_explicit(ptr, memory_order_acquire);
#elif defined(_WIN32) || defined(_WIN64)
  /* Use volatile read + acquire barrier for load */
  int value = *(volatile LONG *)ptr;
  _ReadBarrier();
  return value;
#else
  return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
#endif
}

/**
 * @brief Atomically store value (release semantics)
 */
static inline void t_atomic_store(t_atomic_int_t *ptr, int value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  atomic_store_explicit(ptr, value, memory_order_release);
#elif defined(_WIN32) || defined(_WIN64)
  /* Use release barrier + volatile write for store */
  _WriteBarrier();
  *(volatile LONG *)ptr = (LONG)value;
#else
  __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
#endif
}

/**
 * @brief Atomic compare-and-swap
 * @return 1 if swap succeeded, 0 otherwise
 */
static inline int t_atomic_cas(t_atomic_int_t *ptr, int expected, int desired) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_compare_exchange_strong(ptr, &expected, desired);
#elif defined(_WIN32) || defined(_WIN64)
  return InterlockedCompareExchange(ptr, (LONG)desired, (LONG)expected) == (LONG)expected;
#else
  return __atomic_compare_exchange_n(ptr, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
}

/**
 * @brief Atomically fetch and add, return OLD value
 */
static inline int t_atomic_fetch_add(t_atomic_int_t *ptr, int value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_add(ptr, value);
#elif defined(_WIN32) || defined(_WIN64)
  return (int)InterlockedExchangeAdd(ptr, (LONG)value);
#else
  return __atomic_fetch_add(ptr, value, __ATOMIC_SEQ_CST);
#endif
}

/**
 * @brief Atomically fetch and subtract, return OLD value
 */
static inline int t_atomic_fetch_sub(t_atomic_int_t *ptr, int value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_sub(ptr, value);
#elif defined(_WIN32) || defined(_WIN64)
  return (int)InterlockedExchangeAdd(ptr, -(LONG)value);
#else
  return __atomic_fetch_sub(ptr, value, __ATOMIC_SEQ_CST);
#endif
}

/* ============================================================================
 * Atomic Int64 Operations
 * ============================================================================ */

/**
 * @brief Atomically load 64-bit value (acquire semantics)
 */
static inline int64_t t_atomic_load64(t_atomic_int64_t *ptr) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_load_explicit(ptr, memory_order_acquire);
#elif defined(_WIN32) || defined(_WIN64)
  #ifdef _WIN64
    /* 64-bit: aligned 64-bit reads are atomic */
    int64_t value = *(volatile LONG64 *)ptr;
    _ReadBarrier();
    return value;
  #else
    /* 32-bit: must use interlocked */
    return (int64_t)InterlockedCompareExchange64(ptr, 0, 0);
  #endif
#else
  return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
#endif
}

/**
 * @brief Atomically load 64-bit value (relaxed semantics - no synchronization)
 */
static inline int64_t t_atomic_load64_relaxed(t_atomic_int64_t *ptr) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_load_explicit(ptr, memory_order_relaxed);
#elif defined(_WIN32) || defined(_WIN64)
  /* Volatile read is sufficient for relaxed semantics */
  return *(volatile LONG64 *)ptr;
#else
  return __atomic_load_n(ptr, __ATOMIC_RELAXED);
#endif
}

/**
 * @brief Atomically store 64-bit value (release semantics)
 */
static inline void t_atomic_store64(t_atomic_int64_t *ptr, int64_t value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  atomic_store_explicit(ptr, value, memory_order_release);
#elif defined(_WIN32) || defined(_WIN64)
  #ifdef _WIN64
    /* 64-bit: aligned 64-bit writes are atomic */
    _WriteBarrier();
    *(volatile LONG64 *)ptr = (LONG64)value;
  #else
    /* 32-bit: must use interlocked */
    InterlockedExchange64(ptr, (LONG64)value);
  #endif
#else
  __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
#endif
}

/**
 * @brief Atomically fetch and add to 64-bit value, return OLD value
 */
static inline int64_t t_atomic_fetch_add64(t_atomic_int64_t *ptr, int64_t value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_add(ptr, value);
#elif defined(_WIN32) || defined(_WIN64)
  return (int64_t)InterlockedExchangeAdd64(ptr, (LONG64)value);
#else
  return __atomic_fetch_add(ptr, value, __ATOMIC_SEQ_CST);
#endif
}

/**
 * @brief Atomically fetch and subtract from 64-bit value, return OLD value
 */
static inline int64_t t_atomic_fetch_sub64(t_atomic_int64_t *ptr, int64_t value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_sub(ptr, value);
#elif defined(_WIN32) || defined(_WIN64)
  return (int64_t)InterlockedExchangeAdd64(ptr, -(LONG64)value);
#else
  return __atomic_fetch_sub(ptr, value, __ATOMIC_SEQ_CST);
#endif
}

/* ============================================================================
 * Atomic Uint16 Operations (for packet IDs, etc.)
 * ============================================================================ */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
typedef _Atomic uint16_t t_atomic_uint16_t;
#elif defined(_WIN32) || defined(_WIN64)
typedef volatile SHORT t_atomic_uint16_t;
#else
typedef volatile uint16_t t_atomic_uint16_t;
#endif

/**
 * @brief Atomically fetch and add to 16-bit value, return OLD value
 */
static inline uint16_t t_atomic_fetch_add_uint16(t_atomic_uint16_t *ptr, uint16_t value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_add(ptr, value);
#elif defined(_WIN32) || defined(_WIN64)
  return (uint16_t)_InterlockedExchangeAdd16(ptr, (SHORT)value);
#else
  return __atomic_fetch_add(ptr, value, __ATOMIC_SEQ_CST);
#endif
}

/**
 * @brief Atomically store 16-bit value
 */
static inline void t_atomic_store_uint16(t_atomic_uint16_t *ptr, uint16_t value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  atomic_store(ptr, value);
#elif defined(_WIN32) || defined(_WIN64)
  _InterlockedExchange16(ptr, (SHORT)value);
#else
  __atomic_store_n(ptr, value, __ATOMIC_SEQ_CST);
#endif
}

/**
 * @brief Atomically load 16-bit value
 */
static inline uint16_t t_atomic_load_uint16(t_atomic_uint16_t *ptr) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_load(ptr);
#elif defined(_WIN32) || defined(_WIN64)
  return (uint16_t)_InterlockedOr16(ptr, 0);
#else
  return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
#endif
}

/* ============================================================================
 * Atomic Pointer Operations
 * ============================================================================ */

/**
 * @brief Atomic compare-and-swap for pointers
 * @return 1 if swap succeeded, 0 otherwise
 */
static inline int t_atomic_cas_ptr(void * volatile *ptr, void *expected, void *desired) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_compare_exchange_strong((_Atomic(void *) *)ptr, &expected, desired);
#elif defined(_WIN32) || defined(_WIN64)
  #ifdef _WIN64
    return InterlockedCompareExchangePointer(ptr, desired, expected) == expected;
  #else
    return InterlockedCompareExchange((volatile LONG *)ptr, (LONG)desired, (LONG)expected) == (LONG)expected;
  #endif
#else
  return __atomic_compare_exchange_n(ptr, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
}

/**
 * @brief Atomic exchange for pointers (swap and return old value)
 * @return Old value of pointer
 */
static inline void *t_atomic_exchange_ptr(void * volatile *ptr, void *desired) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_exchange((_Atomic(void *) *)ptr, desired);
#elif defined(_WIN32) || defined(_WIN64)
  #ifdef _WIN64
    return InterlockedExchangePointer(ptr, desired);
  #else
    return (void *)InterlockedExchange((volatile LONG *)ptr, (LONG)desired);
  #endif
#else
  return __atomic_exchange_n(ptr, desired, __ATOMIC_SEQ_CST);
#endif
}

/* ============================================================================
 * Atomic Size_t Operations
 * ============================================================================ */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  typedef _Atomic size_t t_atomic_size_t;
#elif defined(_WIN32) || defined(_WIN64)
  #ifdef _WIN64
    typedef volatile ULONG64 t_atomic_size_t;
  #else
    typedef volatile ULONG t_atomic_size_t;
  #endif
#else
  typedef volatile size_t t_atomic_size_t;
#endif

/**
 * @brief Atomically load size_t value (acquire semantics)
 */
static inline size_t t_atomic_load_size_acquire(t_atomic_size_t *ptr) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_load_explicit(ptr, memory_order_acquire);
#elif defined(_WIN32) || defined(_WIN64)
  #ifdef _WIN64
    size_t value = (size_t)*(volatile ULONG64 *)ptr;
  #else
    size_t value = (size_t)*(volatile ULONG *)ptr;
  #endif
  _ReadBarrier();
  return value;
#else
  return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
#endif
}

/**
 * @brief Atomically load size_t value (relaxed semantics)
 */
static inline size_t t_atomic_load_size_relaxed(t_atomic_size_t *ptr) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_load_explicit(ptr, memory_order_relaxed);
#elif defined(_WIN32) || defined(_WIN64)
  #ifdef _WIN64
    return (size_t)*(volatile ULONG64 *)ptr;
  #else
    return (size_t)*(volatile ULONG *)ptr;
  #endif
#else
  return __atomic_load_n(ptr, __ATOMIC_RELAXED);
#endif
}

/**
 * @brief Atomically store size_t value (release semantics)
 */
static inline void t_atomic_store_size_release(t_atomic_size_t *ptr, size_t value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  atomic_store_explicit(ptr, value, memory_order_release);
#elif defined(_WIN32) || defined(_WIN64)
  _WriteBarrier();
  #ifdef _WIN64
    *(volatile ULONG64 *)ptr = (ULONG64)value;
  #else
    *(volatile ULONG *)ptr = (ULONG)value;
  #endif
#else
  __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
#endif
}

/**
 * @brief Atomically store size_t value (relaxed semantics)
 */
static inline void t_atomic_store_size_relaxed(t_atomic_size_t *ptr, size_t value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  atomic_store_explicit(ptr, value, memory_order_relaxed);
#elif defined(_WIN32) || defined(_WIN64)
  #ifdef _WIN64
    *(volatile ULONG64 *)ptr = (ULONG64)value;
  #else
    *(volatile ULONG *)ptr = (ULONG)value;
  #endif
#else
  __atomic_store_n(ptr, value, __ATOMIC_RELAXED);
#endif
}

/* ============================================================================
 * Atomic Bool Operations
 * ============================================================================ */

typedef t_atomic_int_t t_atomic_bool_t;

/**
 * @brief Atomically load bool value
 */
static inline bool t_atomic_load_bool(t_atomic_bool_t *ptr) {
  return t_atomic_load(ptr) != 0;
}

/**
 * @brief Atomically store bool value
 */
static inline void t_atomic_store_bool(t_atomic_bool_t *ptr, bool value) {
  t_atomic_store(ptr, value ? 1 : 0);
}

/**
 * @brief Atomic compare-and-swap for bool
 * @return 1 if swap succeeded, 0 otherwise
 */
static inline int t_atomic_cas_bool(t_atomic_bool_t *ptr, bool expected, bool desired) {
  return t_atomic_cas(ptr, expected ? 1 : 0, desired ? 1 : 0);
}

/* ============================================================================
 * Atomic Uint32 Operations
 * ============================================================================ */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  typedef _Atomic uint32_t t_atomic_uint32_t;
#elif defined(_WIN32) || defined(_WIN64)
  typedef volatile ULONG t_atomic_uint32_t;
#else
  typedef volatile uint32_t t_atomic_uint32_t;
#endif

/**
 * @brief Atomically load uint32_t value
 */
static inline uint32_t t_atomic_load_uint32(t_atomic_uint32_t *ptr) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_load(ptr);
#elif defined(_WIN32) || defined(_WIN64)
  uint32_t value = *(volatile ULONG *)ptr;
  _ReadBarrier();
  return value;
#else
  return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
#endif
}

/**
 * @brief Atomically store uint32_t value
 */
static inline void t_atomic_store_uint32(t_atomic_uint32_t *ptr, uint32_t value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  atomic_store(ptr, value);
#elif defined(_WIN32) || defined(_WIN64)
  _WriteBarrier();
  *(volatile ULONG *)ptr = (ULONG)value;
#else
  __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
#endif
}

/**
 * @brief Atomically fetch and add to uint32_t value, return OLD value
 */
static inline uint32_t t_atomic_fetch_add_uint32(t_atomic_uint32_t *ptr, uint32_t value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_add(ptr, value);
#elif defined(_WIN32) || defined(_WIN64)
  return (uint32_t)InterlockedExchangeAdd((volatile LONG *)ptr, (LONG)value);
#else
  return __atomic_fetch_add(ptr, value, __ATOMIC_SEQ_CST);
#endif
}

/**
 * @brief Atomically fetch and subtract from uint32_t value, return OLD value
 */
static inline uint32_t t_atomic_fetch_sub_uint32(t_atomic_uint32_t *ptr, uint32_t value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_sub(ptr, value);
#elif defined(_WIN32) || defined(_WIN64)
  return (uint32_t)InterlockedExchangeAdd((volatile LONG *)ptr, -(LONG)value);
#else
  return __atomic_fetch_sub(ptr, value, __ATOMIC_SEQ_CST);
#endif
}

/**
 * @brief Atomic compare-and-swap for uint32_t
 * @return 1 if swap succeeded, 0 otherwise
 */
static inline int t_atomic_cas_uint32(t_atomic_uint32_t *ptr, uint32_t expected, uint32_t desired) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_compare_exchange_strong(ptr, &expected, desired);
#elif defined(_WIN32) || defined(_WIN64)
  return InterlockedCompareExchange((volatile LONG *)ptr, (LONG)desired, (LONG)expected) == (LONG)expected;
#else
  return __atomic_compare_exchange_n(ptr, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
}

/* ============================================================================
 * Atomic Uint64 Operations
 * ============================================================================ */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  typedef _Atomic uint64_t t_atomic_uint64_t;
#elif defined(_WIN32) || defined(_WIN64)
  typedef volatile ULONG64 t_atomic_uint64_t;
#else
  typedef volatile uint64_t t_atomic_uint64_t;
#endif

/**
 * @brief Atomically load uint64_t value
 */
static inline uint64_t t_atomic_load_uint64(t_atomic_uint64_t *ptr) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_load_explicit(ptr, memory_order_acquire);
#elif defined(_WIN32) || defined(_WIN64)
  #ifdef _WIN64
    uint64_t value = *(volatile ULONG64 *)ptr;
    _ReadBarrier();
    return value;
  #else
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)ptr, 0, 0);
  #endif
#else
  return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
#endif
}

/**
 * @brief Atomically store uint64_t value
 */
static inline void t_atomic_store_uint64(t_atomic_uint64_t *ptr, uint64_t value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  atomic_store_explicit(ptr, value, memory_order_release);
#elif defined(_WIN32) || defined(_WIN64)
  #ifdef _WIN64
    _WriteBarrier();
    *(volatile ULONG64 *)ptr = (ULONG64)value;
  #else
    InterlockedExchange64((volatile LONG64 *)ptr, (LONG64)value);
  #endif
#else
  __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
#endif
}

/**
 * @brief Atomically fetch and add to uint64_t value, return OLD value
 */
static inline uint64_t t_atomic_fetch_add_uint64(t_atomic_uint64_t *ptr, uint64_t value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_add(ptr, value);
#elif defined(_WIN32) || defined(_WIN64)
  return (uint64_t)InterlockedExchangeAdd64((volatile LONG64 *)ptr, (LONG64)value);
#else
  return __atomic_fetch_add(ptr, value, __ATOMIC_SEQ_CST);
#endif
}

/**
 * @brief Atomically fetch and subtract from uint64_t value, return OLD value
 */
static inline uint64_t t_atomic_fetch_sub_uint64(t_atomic_uint64_t *ptr, uint64_t value) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_fetch_sub(ptr, value);
#elif defined(_WIN32) || defined(_WIN64)
  return (uint64_t)InterlockedExchangeAdd64((volatile LONG64 *)ptr, -(LONG64)value);
#else
  return __atomic_fetch_sub(ptr, value, __ATOMIC_SEQ_CST);
#endif
}

/**
 * @brief Atomic compare-and-swap for uint64_t
 * @return 1 if swap succeeded, 0 otherwise
 */
static inline int t_atomic_cas_uint64(t_atomic_uint64_t *ptr, uint64_t expected, uint64_t desired) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  return atomic_compare_exchange_strong(ptr, &expected, desired);
#elif defined(_WIN32) || defined(_WIN64)
  return InterlockedCompareExchange64((volatile LONG64 *)ptr, (LONG64)desired, (LONG64)expected) == (LONG64)expected;
#else
  return __atomic_compare_exchange_n(ptr, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
}

/* ============================================================================
 * Initialization Macros
 * ============================================================================ */

#define T_ATOMIC_INIT(value) (value)

#ifdef __cplusplus
}
#endif

#endif /* T_ATOMIC_H */
