/**
 * @file turbo_thread.h
 * @brief Threading primitives and thread pool for TurboNet
 *
 * Cross-platform threading: mutex, condition variable, thread, thread pool.
 * Windows uses SRW Lock + Condition Variable, POSIX uses pthread.
 */

#ifndef TURBO_THREAD_H
#define TURBO_THREAD_H

#include "platform.h"

// =============================================================================
// Threading types
// =============================================================================

// Opaque types for threading
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

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Mutex
// =============================================================================

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

// =============================================================================
// Read-Write Lock - multi-reader / single-writer lock
// =============================================================================

#ifdef _WIN32
typedef struct turbo_rwlock_s {
  SRWLOCK lock;
} turbo_rwlock_t;
#else
typedef struct turbo_rwlock_s {
  pthread_rwlock_t lock;
} turbo_rwlock_t;
#endif

/**
 * @brief Initialize a read-write lock
 * @param lock Lock to initialize
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_rwlock_init(turbo_rwlock_t *lock);

/**
 * @brief Destroy a read-write lock
 * @param lock Lock to destroy
 */
CXX_C_API void turbo_rwlock_destroy(turbo_rwlock_t *lock);

/**
 * @brief Acquire the read lock (shared, multiple readers allowed)
 * @param lock Lock to acquire
 */
CXX_C_API void turbo_rwlock_rdlock(turbo_rwlock_t *lock);

/**
 * @brief Release the read lock
 * @param lock Lock to release
 */
CXX_C_API void turbo_rwlock_rdunlock(turbo_rwlock_t *lock);

/**
 * @brief Acquire the write lock (exclusive, blocks all readers and writers)
 * @param lock Lock to acquire
 */
CXX_C_API void turbo_rwlock_wrlock(turbo_rwlock_t *lock);

/**
 * @brief Release the write lock
 * @param lock Lock to release
 */
CXX_C_API void turbo_rwlock_wrunlock(turbo_rwlock_t *lock);

// =============================================================================
// Condition Variable
// =============================================================================

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
 * @param mutex Mutex to hold while waiting
 */
CXX_C_API void turbo_cond_wait(turbo_cond_t *cond, turbo_mutex_t *mutex);

/**
 * @brief Wait for a condition variable with timeout
 * @param cond Condition variable to wait on
 * @param mutex Mutex to hold while waiting
 * @param timeout_ns Timeout in nanoseconds
 * @return 0 on success, UV_ETIMEDOUT on timeout
 */
CXX_C_API int turbo_cond_timedwait(turbo_cond_t *cond, turbo_mutex_t *mutex, uint64_t timeout_ns);

// =============================================================================
// Thread
// =============================================================================

/**
 * @brief Create a new thread
 * @param thread Thread handle
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
 * @brief Destroy a thread handle (detach if running)
 * @param thread Thread handle
 */
CXX_C_API void turbo_thread_destroy(turbo_thread_t *thread);

/**
 * @brief Run a function exactly once (thread-safe)
 * @param guard Control variable
 * @param callback Function to run
 */
CXX_C_API void turbo_once(turbo_once_t *guard, void (*callback)(void));

/**
 * @brief Sleep for specified milliseconds
 * @param ms Milliseconds to sleep
 */
CXX_C_API void turbo_sleep_ms(uint32_t ms);

/**
 * @brief Yield current thread's time slice to other threads
 */
CXX_C_API void turbo_thread_yield(void);

// =============================================================================
// Global Synchronization Policy
// =============================================================================

/**
 * @brief Enable/disable global locking for all shared resources.
 * @param enabled 0 to disable all internal mutexes (optimizes for single-loop processes).
 */
CXX_C_API void turbo_sync_set_single_threaded(int enabled);

/** @brief Check if we are running in single-threaded mode. */
CXX_C_API int turbo_sync_is_single_threaded(void);

// =============================================================================
// Thread Pool
// =============================================================================

/** Thread pool handle (opaque) */
typedef struct turbo_threadpool_s turbo_threadpool_t;

/** Task callback */
typedef void (*turbo_task_fn)(void *arg);

/**
 * @brief Create a thread pool
 * @param num_threads Number of worker threads (0 = auto-detect CPU cores)
 * @return Thread pool or NULL on failure
 */
CXX_C_API turbo_threadpool_t *turbo_threadpool_create(int num_threads);

/**
 * @brief Destroy thread pool (waits for pending tasks)
 * @param pool Thread pool to destroy
 */
CXX_C_API void turbo_threadpool_destroy(turbo_threadpool_t *pool);

/**
 * @brief Submit a task to the pool
 * @param pool Thread pool
 * @param task Task function
 * @param arg Argument passed to task
 * @return 0 on success, -1 on failure
 */
CXX_C_API int turbo_threadpool_submit(turbo_threadpool_t *pool, turbo_task_fn task, void *arg);

/**
 * @brief Wait for all submitted tasks to complete
 * @param pool Thread pool
 */
CXX_C_API void turbo_threadpool_wait(turbo_threadpool_t *pool);

/**
 * @brief Get number of pending tasks
 * @param pool Thread pool
 * @return Number of tasks waiting + running
 */
CXX_C_API int turbo_threadpool_pending(turbo_threadpool_t *pool);

/**
 * @brief Get number of worker threads
 * @param pool Thread pool
 * @return Number of threads
 */
CXX_C_API int turbo_threadpool_size(turbo_threadpool_t *pool);

/**
 * @brief Get the current process ID
 * @return Process ID
 */
CXX_C_API int turbo_getpid(void);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_THREAD_H */
