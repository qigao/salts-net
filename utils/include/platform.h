/**
 * @file platform.h
 * @brief Minimal cross-platform utilities for TurboUtils
 * @author Follows Linux philosophy: Simple, direct, no bullshit
 *
 * Only what we actually use - pure POSIX / Win32, no third-party dependencies.
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#ifndef _WIN32
  #ifndef _DEFAULT_SOURCE
    #define _DEFAULT_SOURCE 1
  #endif
  #ifndef _POSIX_C_SOURCE
    #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _XOPEN_SOURCE
    #define _XOPEN_SOURCE 700
  #endif
#endif

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
        #define CXX_API                 // Static library or POSIX: no decoration
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
  #include <ws2tcpip.h>
  #include <windows.h>

  // Windows doesn't have ssize_t, define it if not already defined by uv
  #ifndef _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
    #define _SSIZE_T_DEFINED
  #endif
#else
  #include <arpa/inet.h>
  #include <ifaddrs.h>
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
#include <time.h>

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
  int64_t tv_sec;  /**< Seconds since epoch */
  int32_t tv_usec; /**< Microseconds */
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
 * @brief Thread-safe UTC time decomposition.
 * @param t Seconds since Unix epoch
 * @param out Broken-down UTC time
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_gmtime(time_t t, struct tm *out);

/**
 * @brief Thread-safe local time decomposition.
 * @param t Seconds since Unix epoch
 * @param out Broken-down local time
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_localtime(time_t t, struct tm *out);

/**
 * @brief Convert broken-down UTC time to seconds since Unix epoch.
 * @param tm_value Broken-down UTC time
 * @return Seconds since Unix epoch, or (time_t)-1 on invalid input
 */
CXX_C_API time_t turbo_timegm(const struct tm *tm_value);

/**
 * @brief Convert broken-down local time to seconds since Unix epoch.
 * @param tm_value Broken-down local time, normalized by the platform mktime
 * @return Seconds since Unix epoch, or (time_t)-1 on failure
 */
CXX_C_API time_t turbo_mktime(struct tm *tm_value);

/**
 * @brief Format UTC time with strftime semantics.
 * @param t Seconds since Unix epoch
 * @param format strftime format string
 * @param buffer Destination buffer
 * @param buffer_size Size of destination buffer
 * @return Number of bytes written, or negative error code on failure
 */
CXX_C_API int turbo_strftime_utc(time_t t, const char *format, char *buffer,
                                 size_t buffer_size);

/**
 * @brief Format local time with strftime semantics.
 * @param t Seconds since Unix epoch
 * @param format strftime format string
 * @param buffer Destination buffer
 * @param buffer_size Size of destination buffer
 * @return Number of bytes written, or negative error code on failure
 */
CXX_C_API int turbo_strftime_local(time_t t, const char *format, char *buffer,
                                   size_t buffer_size);

/**
 * @brief Maximum platform info string length including trailing NUL
 */
#define TURBO_PLATFORM_INFO_MAX 128

/**
 * @brief Get normalized operating system name
 * @param buffer Destination buffer
 * @param buffer_size Size of destination buffer
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_platform_os_name(char *buffer, size_t buffer_size);

/**
 * @brief Get operating system version string
 * @param buffer Destination buffer
 * @param buffer_size Size of destination buffer
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_platform_os_version(char *buffer, size_t buffer_size);

/**
 * @brief Get normalized machine architecture
 * @param buffer Destination buffer
 * @param buffer_size Size of destination buffer
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_platform_arch(char *buffer, size_t buffer_size);

typedef struct {
  char model[TURBO_PLATFORM_INFO_MAX];
  int core_count;
  double speed_mhz;
} turbo_platform_cpu_info_t;

typedef struct {
  uint64_t total_memory;
  uint64_t free_memory;
  uint64_t available_memory;
} turbo_platform_memory_info_t;

typedef struct {
  double one_minute;
  double five_minutes;
  double fifteen_minutes;
} turbo_platform_load_average_t;

typedef struct {
  char name[TURBO_PLATFORM_INFO_MAX];
  char address[TURBO_PLATFORM_INFO_MAX];
  char netmask[TURBO_PLATFORM_INFO_MAX];
  int is_internal;
} turbo_platform_network_interface_t;

/**
 * @brief Get current username
 * @param buffer Destination buffer
 * @param buffer_size Size of destination buffer
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_platform_username(char *buffer, size_t buffer_size);

/**
 * @brief Get current hostname
 * @param buffer Destination buffer
 * @param buffer_size Size of destination buffer
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_platform_hostname(char *buffer, size_t buffer_size);

/**
 * @brief Get CPU information
 * @param info Output structure
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_platform_cpu_info(turbo_platform_cpu_info_t *info);

/**
 * @brief Get memory information
 * @param info Output structure
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_platform_memory_info(turbo_platform_memory_info_t *info);

/**
 * @brief Get system load average
 * @param info Output structure
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_platform_load_average(turbo_platform_load_average_t *info);

/**
 * @brief Get network interface information
 * @param interfaces Output buffer for interfaces
 * @param max_interfaces Capacity of output buffer
 * @param count Receives number of interfaces written
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_platform_network_interfaces(turbo_platform_network_interface_t *interfaces,
                                                size_t max_interfaces, size_t *count);

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
CXX_C_API int turbo_timer_start(turbo_timer_t *timer, turbo_timer_cb cb, uint64_t timeout,
                                uint64_t repeat);

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
// Cache / Branch Prediction Hints
// =============================================================================
#if defined(__GNUC__) || defined(__clang__)
  #define likely(x) __builtin_expect(!!(x), 1)
  #define unlikely(x) __builtin_expect(!!(x), 0)
#else
  #define likely(x) (x)
  #define unlikely(x) (x)
#endif

#ifdef __cplusplus
}
#endif

#endif // PLATFORM_H
