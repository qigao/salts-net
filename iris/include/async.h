#ifndef IRIS_ASYNC_H
#define IRIS_ASYNC_H

#include "platform.h"
#include "turbo_str.h"
#include "netcore.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Async Task API
// =============================================================================

/** Opaque task handle */
typedef struct iris_async_task_s iris_async_task_t;

/** Task work function (runs in thread pool) */
typedef void (*iris_async_work_fn)(iris_async_task_t *task, void *context);

/** Task completion handler (runs in main thread) */
typedef void (*iris_async_done_fn)(void *context, int success, const char *error);

/**
 * @brief Mark task as successful
 * @param task Task handle
 */
CXX_C_API void iris_async_ok(iris_async_task_t *task);

/**
 * @brief Mark task as failed
 * @param task Task handle
 * @param error Error message
 */
CXX_C_API void iris_async_fail(iris_async_task_t *task, const char *error);

/**
 * @brief Submit async task to thread pool
 * @param context User context passed to callbacks
 * @param work_fn Work function (runs in thread pool)
 * @param done_fn Completion handler (runs after work completes)
 * @return 0 on success, -1 on failure
 */
CXX_C_API int iris_async_submit(void *context, iris_async_work_fn work_fn, iris_async_done_fn done_fn);

/**
 * @brief Chain another task after completion
 * @param context User context
 * @param success Whether previous task succeeded
 * @param error Error from previous task (if failed)
 * @param next_work_fn Next work function
 * @param done_fn Completion handler
 */
CXX_C_API void iris_async_then(void *context, int success, const char *error,
                               iris_async_work_fn next_work_fn, iris_async_done_fn done_fn);

// =============================================================================
// Thread Pool Management
// =============================================================================

/**
 * @brief Initialize the Iris thread pool
 * @param num_threads Number of worker threads (0 = auto-detect)
 * @return 0 on success, -1 on failure
 */
CXX_C_API int iris_async_init(int num_threads);

/**
 * @brief Shutdown the Iris thread pool
 */
CXX_C_API void iris_async_shutdown(void);

/**
 * @brief Wait for all pending tasks to complete
 */
CXX_C_API void iris_async_drain(void);

/**
 * @brief Get number of pending tasks
 * @return Number of tasks in queue + running
 */
CXX_C_API int iris_async_pending(void);

// =============================================================================
// Coroutine-aware blocking await
// =============================================================================

/** Await result - returned by iris_await() */
typedef struct {
    int success;
    const char *error;
} iris_await_result_t;

/**
 * @brief Await a blocking operation from within a coro server handler.
 *
 * Submits work_fn to the thread pool and yields the current coroutine.
 * The coroutine is resumed on the event loop thread when work completes.
 *
 * @param ctx      Event-loop context (from coro_client_get_context)
 * @param work_fn  Work function (runs in thread pool)
 * @param context  User context passed to work_fn
 * @return Result with success/error status
 */
CXX_C_API iris_await_result_t iris_await(coro_context_t *ctx,
                                          iris_async_work_fn work_fn,
                                          void *context);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_ASYNC_H */
