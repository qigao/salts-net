/**
 * @file async.c
 * @brief Iris async task implementation
 *
 * Thread pool for blocking work. iris_await() bridges thread pool completions
 * back to the event loop via coro_post(), so it works correctly inside
 * coro server handlers.
 */

#include "async.h"
#include "turbo_thread.h"
#include "turbo_coro.h"
#include "tlog.h"
#include <stdlib.h>
#include <string.h>

// =============================================================================
// Task Structure
// =============================================================================

struct iris_async_task_s {
    void *context;
    iris_async_work_fn work_fn;
    iris_async_done_fn done_fn;
    int result;
    char *error;
    coro_t *awaiting_coro;
    coro_context_t *coro_ctx;
};

// =============================================================================
// Globals
// =============================================================================

static turbo_threadpool_t *g_pool = NULL;
static turbo_mutex_t g_pool_mutex;
static int g_pool_initialized = 0;

// =============================================================================
// Internal
// =============================================================================

static void task_worker(void *arg) {
    iris_async_task_t *task = (iris_async_task_t *)arg;

    if (task->work_fn) {
        task->work_fn(task, task->context);
    }

    if (task->awaiting_coro) {
        coro_post(task->coro_ctx, (coro_post_fn)coro_resume,
                        task->awaiting_coro, NULL);
    } else if (task->done_fn) {
        task->done_fn(task->context, task->result, task->error);
        if (task->error) free(task->error);
        free(task);
    }
}

static int ensure_pool(void) {
    if (g_pool_initialized) {
        return 0;
    }
    return iris_async_init(0);
}

// =============================================================================
// Thread Pool API
// =============================================================================

int iris_async_init(int num_threads) {
    if (g_pool_initialized) return 0;

    turbo_mutex_init(&g_pool_mutex);

    g_pool = turbo_threadpool_create(num_threads);
    if (!g_pool) {
        TLOG_ERROR("Failed to create Iris thread pool");
        turbo_mutex_destroy(&g_pool_mutex);
        return -1;
    }

    g_pool_initialized = 1;
    TLOG_INFO("Iris async initialized: {} workers", turbo_threadpool_size(g_pool));
    return 0;
}

void iris_async_shutdown(void) {
    if (!g_pool_initialized) return;

    iris_async_drain();

    turbo_mutex_lock(&g_pool_mutex);
    if (g_pool) {
        turbo_threadpool_destroy(g_pool);
        g_pool = NULL;
    }
    turbo_mutex_unlock(&g_pool_mutex);

    turbo_mutex_destroy(&g_pool_mutex);

    g_pool_initialized = 0;
}

void iris_async_drain(void) {
    if (!g_pool) return;
    turbo_threadpool_wait(g_pool);
}

int iris_async_pending(void) {
    if (!g_pool) return 0;
    return turbo_threadpool_pending(g_pool);
}

void iris_async_ok(iris_async_task_t *task) {
    if (task) task->result = 1;
}

void iris_async_fail(iris_async_task_t *task, const char *error) {
    if (!task) return;
    task->result = 0;
    if (task->error) free(task->error);
    task->error = error ? strdup(error) : strdup("Unknown error");
}

int iris_async_submit(void *context, iris_async_work_fn work_fn, iris_async_done_fn done_fn) {
    if (!work_fn) return -1;
    if (ensure_pool() != 0 || !g_pool) return -1;

    iris_async_task_t *task = calloc(1, sizeof(iris_async_task_t));
    if (!task) return -1;

    task->context = context;
    task->work_fn = work_fn;
    task->done_fn = done_fn;

    if (turbo_threadpool_submit(g_pool, task_worker, task) != 0) {
        free(task);
        return -1;
    }
    return 0;
}

void iris_async_then(void *context, int success, const char *error,
                     iris_async_work_fn next_work_fn, iris_async_done_fn done_fn) {
    if (success) {
        if (iris_async_submit(context, next_work_fn, done_fn) != 0 && done_fn) {
            done_fn(context, 0, "submit failed");
        }
    } else if (done_fn) {
        done_fn(context, 0, error);
    }
}

// =============================================================================
// Coroutine-aware await
// =============================================================================

iris_await_result_t iris_await(coro_context_t *ctx,
                               iris_async_work_fn work_fn, void *context) {
    iris_await_result_t fail = {0, "not in coroutine"};
    coro_t *co = coro_running();
    if (!co || !work_fn || !ctx) return fail;

    if (ensure_pool() != 0 || !g_pool) {
        fail.error = "async init failed";
        return fail;
    }

    iris_async_task_t *task = calloc(1, sizeof(iris_async_task_t));
    if (!task) {
        fail.error = "alloc failed";
        return fail;
    }

    task->context = context;
    task->work_fn = work_fn;
    task->awaiting_coro = co;
    task->coro_ctx = ctx;

    if (turbo_threadpool_submit(g_pool, task_worker, task) != 0) {
        free(task);
        fail.error = "submit failed";
        return fail;
    }

    coro_yield();

    iris_await_result_t result;
    result.success = task->result;
    result.error = task->error;

    char *err_copy = task->error ? strdup(task->error) : NULL;
    if (task->error) free(task->error);
    free(task);

    result.error = err_copy;
    return result;
}
