/**
 * @file platform.h
 * @brief Minimal cross-platform utilities for TurboNet
 * @author Follows Linux philosophy: Simple, direct, no bullshit
 *
 * Only what we actually use - libuv handles the heavy lifting
 */
#ifndef TURBONET_PLATFORM_H
#define TURBONET_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// =============================================================================
// DLL Export/Import macros for Windows dynamic builds
// =============================================================================
#ifdef _WIN32
  #ifdef TURBONET_BUILDING_DLL
    #define TURBONET_API __declspec(dllexport)
  #elif defined(TURBONET_USING_DLL)
    #define TURBONET_API __declspec(dllimport)
  #else
    #define TURBONET_API
  #endif
#else
  #define TURBONET_API
#endif

// Platform detection - keep it simple
#ifdef _WIN32
  #define TURBO_WIN32 1
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>

// Windows doesn't have ssize_t, define it
typedef intptr_t ssize_t;

// Windows threading
typedef CRITICAL_SECTION turbo_mutex_t;

// Cross-platform string duplication
#define turbo_strdup _strdup

#else
  #define TURBO_UNIX 1
  #include <pthread.h>
  #include <unistd.h>  // For ssize_t on Unix

// Unix threading
typedef pthread_mutex_t turbo_mutex_t;

// Cross-platform string duplication
#define turbo_strdup strdup

#endif

// =============================================================================
// Threading utilities
// =============================================================================

/**
 * @brief Initialize a mutex for thread synchronization
 */
TURBONET_API int turbo_mutex_init(turbo_mutex_t* mutex);

/**
 * @brief Lock a mutex
 */
TURBONET_API int turbo_mutex_lock(turbo_mutex_t* mutex);

/**
 * @brief Unlock a mutex
 */
TURBONET_API int turbo_mutex_unlock(turbo_mutex_t* mutex);

/**
 * @brief Destroy a mutex and free its resources
 */
TURBONET_API int turbo_mutex_destroy(turbo_mutex_t* mutex);

// =============================================================================
// Time utilities - platform-independent high-resolution timing
// =============================================================================

/**
 * @brief Get current time in milliseconds since epoch
 * @return Current time in milliseconds
 */
TURBONET_API uint64_t turbo_now_ms(void);

/**
 * @brief Get current high-resolution time in nanoseconds
 * @return Current time in nanoseconds (monotonic)
 */
TURBONET_API uint64_t turbo_hrtime(void);

/**
 * @brief Sleep for specified number of milliseconds
 * @param ms Number of milliseconds to sleep
 * 
 * @note This is blocking sleep, use timers for non-blocking delays
 */
TURBONET_API void turbo_sleep_ms(uint32_t ms);

/**
 * @brief Get uptime in milliseconds since process start
 * @return Process uptime in milliseconds
 */
TURBONET_API uint64_t turbo_uptime_ms(void);

/**
 * @brief Convert nanoseconds to milliseconds
 */
static inline uint64_t turbo_ns_to_ms(uint64_t ns) {
    return ns / 1000000ULL;
}

/**
 * @brief Convert milliseconds to nanoseconds
 */
static inline uint64_t turbo_ms_to_ns(uint64_t ms) {
    return ms * 1000000ULL;
}

// =============================================================================
// Timer utilities - cross-platform async timers
// =============================================================================

// Forward declaration - implementation is opaque
typedef struct turbo_timer_s turbo_timer_t;
typedef void (*turbo_timer_cb)(turbo_timer_t* timer);

/**
 * @brief Initialize a timer with global event loop
 * @param timer Timer to initialize
 * @return 0 on success, error code on failure
 */
TURBONET_API int turbo_timer_init(turbo_timer_t* timer);

/**
 * @brief Start a timer
 * @param timer Timer to start
 * @param cb Callback to call when timer fires
 * @param timeout Timeout in milliseconds
 * @param repeat Repeat interval in milliseconds (0 for one-shot)
 * @return 0 on success, error code on failure
 */
TURBONET_API int turbo_timer_start(turbo_timer_t* timer, turbo_timer_cb cb, uint64_t timeout, uint64_t repeat);

/**
 * @brief Stop a timer
 * @param timer Timer to stop
 * @return 0 on success, error code on failure
 */
TURBONET_API int turbo_timer_stop(turbo_timer_t* timer);

/**
 * @brief Close a timer and free resources
 * @param timer Timer to close
 */
TURBONET_API void turbo_timer_close(turbo_timer_t* timer);

/**
 * @brief Set timer user data
 * @param timer Timer to set data on
 * @param data User data pointer
 */
TURBONET_API void turbo_timer_set_data(turbo_timer_t* timer, void* data);

/**
 * @brief Get timer user data
 * @param timer Timer to get data from
 * @return User data pointer
 */
TURBONET_API void* turbo_timer_get_data(turbo_timer_t* timer);

/**
 * @brief Get remaining time until timer fires
 * @param timer Timer to query
 * @return Remaining time in milliseconds, 0 if not running
 */
TURBONET_API uint64_t turbo_timer_get_due_in(turbo_timer_t* timer);

/**
 * @brief Get timer repeat interval
 * @param timer Timer to query
 * @return Repeat interval in milliseconds
 */
TURBONET_API uint64_t turbo_timer_get_repeat(turbo_timer_t* timer);

/**
 * @brief Mark a variable as unused to suppress compiler warnings
 */
#define UNUSED(x) (void)(x)

#endif  // TURBONET_PLATFORM_H
