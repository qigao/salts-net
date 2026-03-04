/**
 * @file platform.h
 * @brief Minimal cross-platform utilities for TurboNet
 * @author Follows Linux philosophy: Simple, direct, no bullshit
 *
 * Only what we actually use - libuv handles the heavy lifting
 */

#ifndef TURBONET_PLATFORM_H
#define TURBONET_PLATFORM_H

// =============================================================================
// DLL Export/Import macros for cross-platform shared library builds
// =============================================================================
// clang-format off
#define CXX_EXTERN_C extern "C"
#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
    #define CXX_DLL_IMPORT __declspec(dllimport)
    #define CXX_DLL_EXPORT __declspec(dllexport)
    #define CXX_DLL_LOCAL
#else
    #if defined(__GNUC__) && __GNUC__ >= 4
        #define CXX_DLL_IMPORT __attribute__((visibility("default")))
        #define CXX_DLL_EXPORT __attribute__((visibility("default")))
        #define CXX_DLL_LOCAL __attribute__((visibility("hidden")))
    #else
        #define CXX_DLL_IMPORT
        #define CXX_DLL_EXPORT
        #define CXX_DLL_LOCAL
    #endif
#endif

#ifndef CXX_API
    #if defined(SHARED_CXX)
        #define CXX_API CXX_DLL_EXPORT  // Building DLL: export symbols
    #else
        #define CXX_API                 // Static library or importing: no decoration
    #endif
#endif
#ifdef __cplusplus
    #define CXX_C_API CXX_EXTERN_C CXX_API
#else
    #define CXX_C_API CXX_API
#endif

// clang-format on

#ifdef _WIN32
  #define TURBO_WIN32 1
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  // uv.h includes windows.h, so we might not need to explicit include it,
  // but keeping it for other utils if needed.
  // Since we hid uv.h, we MUST include windows.h now for LONG, etc.
  #include <winsock2.h>
  #include <windows.h>

  // Windows doesn't have ssize_t, define it if not already defined by uv
  #ifndef _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
    #define _SSIZE_T_DEFINED
  #endif
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <strings.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Time utilities - platform-independent high-resolution timing
// =============================================================================

/**
 * @brief Get monotonic time in milliseconds (never goes backward)
 * @return Monotonic time in milliseconds, suitable for measuring intervals
 */
CXX_C_API uint64_t turbo_monotonic_ms(void);

/**
 * @brief Get real time in milliseconds since Unix epoch
 * @return Wall clock time in milliseconds (can jump if system time changes)
 */
CXX_C_API uint64_t turbo_realtime_ms(void);

/**
 * @brief Cross-platform time structure (Y2038 safe)
 */
typedef struct {
    int64_t tv_sec;   /**< Seconds since epoch */
    int32_t tv_usec;  /**< Microseconds */
} turbo_timeval_t;

/**
 * @brief Cross-platform timezone structure (usually ignored)
 */
typedef struct {
    int tz_minuteswest;
    int tz_dsttime;
} turbo_timezone_t;

/**
 * @brief Cross-platform gettimeofday equivalent
 * @param tv Timeval structure to fill
 * @param tz Timezone structure (can be NULL)
 * @return 0 on success
 */
CXX_C_API int turbo_gettimeofday(turbo_timeval_t *tv, turbo_timezone_t *tz);

/**
 * @brief Get current high-resolution time in nanoseconds
 * @return Current time in nanoseconds (monotonic)
 */
CXX_C_API uint64_t turbo_hrtime(void);

/**
 * @brief Get uptime in milliseconds since process start
 * @return Process uptime in milliseconds
 */
CXX_C_API uint64_t turbo_uptime_ms(void);

/**
 * @brief Convert nanoseconds to milliseconds
 */
static inline uint64_t turbo_ns_to_ms(uint64_t ns) { return ns / 1000000ULL; }

/**
 * @brief Convert milliseconds to nanoseconds
 */
static inline uint64_t turbo_ms_to_ns(uint64_t ms) { return ms * 1000000ULL; }

// =============================================================================
// Timer utilities - cross-platform async timers (Native OS backend)
// =============================================================================

/**
 * @brief Cross-platform timer using native OS facilities (CreateTimerQueueTimer/timer_create)
 * 
 * This timer does NOT depend on a libuv loop. Callbacks are executed by the OS
 * thread pool (Windows) or a dedicated thread (POSIX), so they must be thread-safe.
 */
typedef struct turbo_native_timer_s turbo_timer_t;
typedef void (*turbo_timer_cb)(turbo_timer_t *timer);

/**
 * @brief Create a timer
 * @param loop Event loop (IGNORED - kept for API compatibility)
 * @return Timer pointer on success, NULL on failure
 */
CXX_C_API turbo_timer_t *turbo_timer_create(void *loop);

/**
 * @brief Destroy a timer and free resources
 * @param timer Timer to destroy (stops if running)
 */
CXX_C_API void turbo_timer_destroy(turbo_timer_t *timer);

/**
 * @brief Start a timer
 * @param timer Timer to start
 * @param cb Callback to call when timer fires (thread-safe!)
 * @param timeout Timeout in milliseconds
 * @param repeat Repeat interval in milliseconds (0 for one-shot)
 * @return 0 on success, error code on failure
 */
CXX_C_API int turbo_timer_start(turbo_timer_t *timer, turbo_timer_cb cb,
                                uint64_t timeout, uint64_t repeat);

/**
 * @brief Stop a timer
 * @param timer Timer to stop
 * @return 0 on success, error code on failure
 */
CXX_C_API int turbo_timer_stop(turbo_timer_t *timer);

/**
 * @brief Set timer user data
 * @param timer Timer to set data on
 * @param data User data pointer
 */
CXX_C_API void turbo_timer_set_data(turbo_timer_t *timer, void *data);

/**
 * @brief Get timer user data
 * @param timer Timer to get data from
 * @return User data pointer
 */
CXX_C_API void *turbo_timer_get_data(turbo_timer_t *timer);

/**
 * @brief Get timer repeat interval
 * @param timer Timer to query
 * @return Repeat interval in milliseconds
 */
CXX_C_API uint64_t turbo_timer_get_repeat(turbo_timer_t *timer);

// =============================================================================
// Read-Write Lock - cross-platform rwlock abstraction
// =============================================================================

#ifdef _WIN32
typedef struct turbo_rwlock_s {
    SRWLOCK lock;
} turbo_rwlock_t;
#else
#include <pthread.h>
typedef struct turbo_rwlock_s {
    pthread_rwlock_t lock;
} turbo_rwlock_t;
#endif

CXX_C_API int  turbo_rwlock_init(turbo_rwlock_t *lock);
CXX_C_API void turbo_rwlock_destroy(turbo_rwlock_t *lock);
CXX_C_API void turbo_rwlock_rdlock(turbo_rwlock_t *lock);
CXX_C_API void turbo_rwlock_rdunlock(turbo_rwlock_t *lock);
CXX_C_API void turbo_rwlock_wrlock(turbo_rwlock_t *lock);
CXX_C_API void turbo_rwlock_wrunlock(turbo_rwlock_t *lock);

/**
 * @brief Mark a variable as unused to suppress compiler warnings
 */
#define UNUSED(x) (void)(x)
 
// =============================================================================
// Cache / Branch Prediction Hints
// =============================================================================
#if defined(__GNUC__) || defined(__clang__)
  #define likely(x)       __builtin_expect(!!(x), 1)
  #define unlikely(x)     __builtin_expect(!!(x), 0)
#else
  #define likely(x)       (x)
  #define unlikely(x)     (x)
#endif

// =============================================================================
// String utilities - safe string duplication with padding for stb_sprintf
// =============================================================================

/**
 * @brief Duplicate a string using a memory pool.
 *
 * @param pool Memory pool to allocate from (MemoryPool*)
 * @param str String to duplicate
 * @return Duplicated string, or NULL on failure
 * @note Memory is managed by the pool, do not free() directly
 */
CXX_C_API char *turbo_memory_pool_strdup(void *pool, const char *str);

/**
 * @brief URL-encode a string per RFC 3986.
 *
 * Encodes special characters as %XX hex sequences. Unreserved characters
 * (A-Z, a-z, 0-9, -, _, ., ~) are left as-is. Spaces become '+'.
 *
 * @param str String to encode
 * @return URL-encoded string with padding, or NULL on failure
 * @note Caller must free() the returned string
 */
CXX_C_API char *turbo_url_encode(const char *str);

/**
 * @brief URL-decode a string per RFC 3986.
 *
 * Decodes %XX hex sequences back to bytes. '+' becomes space.
 *
 * @param str String to decode
 * @return URL-decoded string, or NULL on failure
 * @note Caller must free() the returned string
 */
CXX_C_API char *turbo_url_decode(const char *str);

/**
 * @brief Get the current process ID
 * @return Process ID
 */
CXX_C_API int turbo_getpid(void);

#ifdef __cplusplus
}
#endif

#endif // TURBONET_PLATFORM_H
