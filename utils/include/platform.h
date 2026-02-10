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

  // Cross-platform string duplication
  #define strdup _strdup
  #define strcasecmp _stricmp
  #define strncasecmp _strnicmp
  #define strtok_r strtok_s
#else
  #define TURBO_UNIX 1
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
// Threading primitives - backed by libuv
// =============================================================================

// Opaque types to hide internal implementation (libuv)
// Opaque types to hide internal implementation (libuv)
typedef void *turbo_mutex_t;
typedef void *turbo_cond_t;
typedef void *turbo_thread_t;
// One-time initialization guard
#ifdef _WIN32
typedef INIT_ONCE turbo_once_t;
#define TURBO_ONCE_INIT INIT_ONCE_STATIC_INIT
#else
#include <pthread.h>
typedef pthread_once_t turbo_once_t;
#define TURBO_ONCE_INIT PTHREAD_ONCE_INIT
#endif

// Thread entry point callback type
typedef void (*turbo_thread_cb)(void *arg);

/**
 * @brief Initialize a mutex
 * @param mutex Mutex to initialize
 */
CXX_C_API void turbo_mutex_init(turbo_mutex_t *mutex);

/**
 * @brief Destroy a mutex
 * @param mutex Mutex to destroy
 */
CXX_C_API void turbo_mutex_destroy(turbo_mutex_t *mutex);

/**
 * @brief Lock a mutex
 * @param mutex Mutex to lock
 */
CXX_C_API void turbo_mutex_lock(turbo_mutex_t *mutex);

/**
 * @brief Unlock a mutex
 * @param mutex Mutex to unlock
 */
CXX_C_API void turbo_mutex_unlock(turbo_mutex_t *mutex);

/**
 * @brief Initialize a condition variable
 * @param cond Condition variable to initialize
 */
CXX_C_API void turbo_cond_init(turbo_cond_t *cond);

/**
 * @brief Destroy a condition variable
 * @param cond Condition variable to destroy
 */
CXX_C_API void turbo_cond_destroy(turbo_cond_t *cond);

/**
 * @brief Signal a condition variable (wake one waiting thread)
 * @param cond Condition variable to signal
 */
CXX_C_API void turbo_cond_signal(turbo_cond_t *cond);

/**
 * @brief Broadcast a condition variable (wake all waiting threads)
 * @param cond Condition variable to broadcast
 */
CXX_C_API void turbo_cond_broadcast(turbo_cond_t *cond);

/**
 * @brief Wait for a condition variable
 * @param cond Condition variable to wait on
 * @param mutex Mutex to hold while waiting (released while waiting, re-acquired before return)
 */
CXX_C_API void turbo_cond_wait(turbo_cond_t *cond, turbo_mutex_t *mutex);

/**
 * @brief Wait for a condition variable with timeout
 * @param cond Condition variable to wait on
 * @param mutex Mutex to hold while waiting (released while waiting, re-acquired before return)
 * @param timeout_ns Timeout in nanoseconds
 * @return 0 on success, UV_ETIMEDOUT on timeout
 */
CXX_C_API int turbo_cond_timedwait(turbo_cond_t *cond, turbo_mutex_t *mutex, uint64_t timeout_ns);

// =============================================================================
// Thread utilities - cross-platform threading (via libuv)
// =============================================================================

/**
 * @brief Create a new thread
 * @param thread Thread handle (allocated by caller, initialized by function)
 * @param entry Entry point function
 * @param arg Argument passed to entry point
 * @return 0 on success, < 0 on failure
 */
CXX_C_API int turbo_thread_create(turbo_thread_t *thread, turbo_thread_cb entry, void *arg);

/**
 * @brief Wait for a thread to terminate
 * @param thread Thread handle
 * @return 0 on success, < 0 on failure
 */
CXX_C_API int turbo_thread_join(turbo_thread_t *thread);

/**
 * @brief Run a function exactly once
 * @param guard Control variable
 * @param callback Function to run
 */
CXX_C_API void turbo_once(turbo_once_t *guard, void (*callback)(void));

/**
 * @brief Destroy a thread handle (frees memory)
 * @param thread Thread handle
 */
CXX_C_API void turbo_thread_destroy(turbo_thread_t *thread);

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
 * @brief Sleep for specified number of milliseconds
 * @param ms Number of milliseconds to sleep
 *
 * @note This is blocking sleep, use timers for non-blocking delays
 */
CXX_C_API void turbo_sleep_ms(uint32_t ms);

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

/**
 * @brief Mark a variable as unused to suppress compiler warnings
 */
#define UNUSED(x) (void)(x)

// =============================================================================
// String utilities - safe string duplication with padding for stb_sprintf
// =============================================================================

/**
 * @brief Duplicate a string with padding for stb_sprintf safety.
 *
 * stb_sprintf reads 4 bytes at a time, so strings need padding after
 * the null terminator to avoid buffer overreads.
 *
 * @param s String to duplicate
 * @return Duplicated string with 8 bytes padding, or NULL on failure
 * @note Caller must sdsfree() the returned string
 */
CXX_C_API char *turbo_strdup_padded(const char *s);

/**
 * @brief Duplicate a string using a memory pool.
 *
 * @param pool Memory pool to allocate from (MemoryPool*)
 * @param str String to duplicate
 * @return Duplicated string, or NULL on failure
 * @note Memory is managed by the pool, do not free() directly
 */
CXX_C_API char *turbo_pool_strdup(void *pool, const char *str);

/**
 * @brief Duplicate a string using a memory pool with padding for stb_sprintf.
 *
 * @param pool Memory pool to allocate from (MemoryPool*)
 * @param str String to duplicate
 * @return Duplicated string with padding, or NULL on failure
 * @note Memory is managed by the pool, do not free() directly
 */
CXX_C_API char *turbo_pool_strdup_padded(void *pool, const char *str);

/**
 * @brief Allocate memory with padding for stb_sprintf safety.
 *
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory (size + 8 bytes), or NULL on failure
 * @note Caller must free() the returned pointer
 */
CXX_C_API void *turbo_malloc_padded(size_t size);

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
 * @brief Get the current process ID
 * @return Process ID
 */
CXX_C_API int turbo_getpid(void);

#ifdef __cplusplus
}
#endif

#endif // TURBONET_PLATFORM_H
