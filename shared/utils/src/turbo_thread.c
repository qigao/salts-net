/**
 * @file turbo_thread.c
 * @brief Threading primitives and thread pool implementation
 *
 * Cross-platform: Windows SRW Lock + Condition Variable, POSIX pthread.
 * Thread pool uses lock-free Disruptor + Object Pool for high performance.
 */

#include "turbo_thread.h"
#include "disruptor.h"
#include "object_pool.h"
#include "turbo_atomic.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#endif

// Error codes
#ifndef UV_EINVAL
  #define UV_EINVAL (-22)
#endif
#ifndef UV_ETIMEDOUT
  #define UV_ETIMEDOUT (-110)
#endif
#ifndef UV_ENOMEM
  #define UV_ENOMEM (-12)
#endif

// =============================================================================
// Mutex - Windows
// =============================================================================

#ifdef _WIN32

void turbo_mutex_init(turbo_mutex_t *mutex) {
    if (mutex == NULL) return;
    PSRWLOCK srw_lock = malloc(sizeof(SRWLOCK));
    if (srw_lock == NULL) return;
    InitializeSRWLock(srw_lock);
    *mutex = srw_lock;
}

void turbo_mutex_destroy(turbo_mutex_t *mutex) {
    if (mutex == NULL || *mutex == NULL) return;
    free(*mutex);
    *mutex = NULL;
}

void turbo_mutex_lock(turbo_mutex_t *mutex) {
    if (mutex == NULL || *mutex == NULL) return;
    AcquireSRWLockExclusive((PSRWLOCK)*mutex);
}

void turbo_mutex_unlock(turbo_mutex_t *mutex) {
    if (mutex == NULL || *mutex == NULL) return;
    // Debug: catch invalid mutex pointer
    if ((uintptr_t)*mutex == 0xFFFFFFFFFFFFFFFF || (uintptr_t)*mutex == 0xCDCDCDCDCDCDCDCD) {
        // Invalid mutex pointer detected!
        return;
    }
    ReleaseSRWLockExclusive((PSRWLOCK)*mutex);
}

// =============================================================================
// Condition Variable - Windows
// =============================================================================

void turbo_cond_init(turbo_cond_t *cond) {
    if (cond == NULL) return;
    PCONDITION_VARIABLE cv = malloc(sizeof(CONDITION_VARIABLE));
    if (cv == NULL) return;
    InitializeConditionVariable(cv);
    *cond = cv;
}

void turbo_cond_destroy(turbo_cond_t *cond) {
    if (cond == NULL || *cond == NULL) return;
    free(*cond);
    *cond = NULL;
}

void turbo_cond_signal(turbo_cond_t *cond) {
    if (cond == NULL || *cond == NULL) return;
    WakeConditionVariable((PCONDITION_VARIABLE)*cond);
}

void turbo_cond_broadcast(turbo_cond_t *cond) {
    if (cond == NULL || *cond == NULL) return;
    WakeAllConditionVariable((PCONDITION_VARIABLE)*cond);
}

void turbo_cond_wait(turbo_cond_t *cond, turbo_mutex_t *mutex) {
    if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL) return;
    SleepConditionVariableSRW((PCONDITION_VARIABLE)*cond, (PSRWLOCK)*mutex, INFINITE, 0);
}

int turbo_cond_timedwait(turbo_cond_t *cond, turbo_mutex_t *mutex, uint64_t timeout_ns) {
    if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL) return UV_EINVAL;
    DWORD timeout_ms = (DWORD)(timeout_ns / 1000000ULL);
    BOOL result = SleepConditionVariableSRW((PCONDITION_VARIABLE)*cond, (PSRWLOCK)*mutex, timeout_ms, 0);
    return result ? 0 : UV_ETIMEDOUT;
}

// =============================================================================
// Once - Windows
// =============================================================================

static BOOL CALLBACK InitOnceCallback(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) {
    UNUSED(InitOnce);
    UNUSED(Context);
    void (*callback)(void) = (void (*)(void))Parameter;
    callback();
    return TRUE;
}

void turbo_once(turbo_once_t *guard, void (*callback)(void)) {
    InitOnceExecuteOnce(guard, InitOnceCallback, (PVOID)callback, NULL);
}

// =============================================================================
// Thread - Windows
// =============================================================================

struct turbo_thread_wrapper_ctx {
    turbo_thread_cb entry;
    void *arg;
};

static unsigned __stdcall turbo_thread_entry_wrapper(void *arg) {
    struct turbo_thread_wrapper_ctx *ctx = (struct turbo_thread_wrapper_ctx *)arg;
    turbo_thread_cb entry = ctx->entry;
    void *real_arg = ctx->arg;
    free(ctx);
    entry(real_arg);
    return 0;
}

int turbo_thread_create(turbo_thread_t *thread, turbo_thread_cb entry, void *arg) {
    if (thread == NULL || entry == NULL) return UV_EINVAL;

    struct turbo_thread_wrapper_ctx *ctx = malloc(sizeof(struct turbo_thread_wrapper_ctx));
    if (!ctx) return UV_ENOMEM;
    ctx->entry = entry;
    ctx->arg = arg;

    HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, turbo_thread_entry_wrapper, ctx, 0, NULL);
    if (hThread == NULL) {
        free(ctx);
        return -1;
    }

    *thread = (turbo_thread_t)hThread;
    return 0;
}

int turbo_thread_join(turbo_thread_t *thread) {
    if (thread == NULL || *thread == NULL) return UV_EINVAL;
    HANDLE hThread = (HANDLE)*thread;
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
    *thread = NULL;
    return 0;
}

void turbo_thread_destroy(turbo_thread_t *thread) {
    if (thread == NULL || *thread == NULL) return;
    HANDLE hThread = (HANDLE)*thread;
    CloseHandle(hThread);
    *thread = NULL;
}

void turbo_sleep_ms(uint32_t ms) {
    Sleep(ms);
}

void turbo_thread_yield(void) {
    SwitchToThread();
}

#else

// =============================================================================
// Mutex - POSIX
// =============================================================================

void turbo_mutex_init(turbo_mutex_t *mutex) {
    if (mutex == NULL) return;
    pthread_mutex_t *pthread_mutex = malloc(sizeof(pthread_mutex_t));
    if (pthread_mutex == NULL) return;
    pthread_mutex_init(pthread_mutex, NULL);
    *mutex = pthread_mutex;
}

void turbo_mutex_destroy(turbo_mutex_t *mutex) {
    if (mutex == NULL || *mutex == NULL) return;
    pthread_mutex_t *pthread_mutex = (pthread_mutex_t *)*mutex;
    pthread_mutex_destroy(pthread_mutex);
    free(pthread_mutex);
    *mutex = NULL;
}

void turbo_mutex_lock(turbo_mutex_t *mutex) {
    if (mutex == NULL || *mutex == NULL) return;
    pthread_mutex_lock((pthread_mutex_t *)*mutex);
}

void turbo_mutex_unlock(turbo_mutex_t *mutex) {
    if (mutex == NULL || *mutex == NULL) return;
    pthread_mutex_unlock((pthread_mutex_t *)*mutex);
}

// =============================================================================
// Condition Variable - POSIX
// =============================================================================

void turbo_cond_init(turbo_cond_t *cond) {
    if (cond == NULL) return;
    pthread_cond_t *pthread_cond = malloc(sizeof(pthread_cond_t));
    if (pthread_cond == NULL) return;
    pthread_cond_init(pthread_cond, NULL);
    *cond = pthread_cond;
}

void turbo_cond_destroy(turbo_cond_t *cond) {
    if (cond == NULL || *cond == NULL) return;
    pthread_cond_t *pthread_cond = (pthread_cond_t *)*cond;
    pthread_cond_destroy(pthread_cond);
    free(pthread_cond);
    *cond = NULL;
}

void turbo_cond_signal(turbo_cond_t *cond) {
    if (cond == NULL || *cond == NULL) return;
    pthread_cond_signal((pthread_cond_t *)*cond);
}

void turbo_cond_broadcast(turbo_cond_t *cond) {
    if (cond == NULL || *cond == NULL) return;
    pthread_cond_broadcast((pthread_cond_t *)*cond);
}

void turbo_cond_wait(turbo_cond_t *cond, turbo_mutex_t *mutex) {
    if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL) return;
    pthread_cond_wait((pthread_cond_t *)*cond, (pthread_mutex_t *)*mutex);
}

int turbo_cond_timedwait(turbo_cond_t *cond, turbo_mutex_t *mutex, uint64_t timeout_ns) {
    if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL) return UV_EINVAL;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    ts.tv_nsec += timeout_ns;
    if (ts.tv_nsec >= 1000000000ULL) {
        ts.tv_sec += ts.tv_nsec / 1000000000ULL;
        ts.tv_nsec %= 1000000000ULL;
    }

    int result = pthread_cond_timedwait((pthread_cond_t *)*cond, (pthread_mutex_t *)*mutex, &ts);
    return (result == ETIMEDOUT) ? UV_ETIMEDOUT : 0;
}

// =============================================================================
// Once - POSIX
// =============================================================================

void turbo_once(turbo_once_t *guard, void (*callback)(void)) {
    pthread_once(guard, callback);
}

// =============================================================================
// Thread - POSIX
// =============================================================================

struct turbo_thread_wrapper_ctx {
    turbo_thread_cb entry;
    void *arg;
};

static void *turbo_thread_entry_wrapper_pthread(void *arg) {
    struct turbo_thread_wrapper_ctx *ctx = (struct turbo_thread_wrapper_ctx *)arg;
    turbo_thread_cb entry = ctx->entry;
    void *real_arg = ctx->arg;
    free(ctx);
    entry(real_arg);
    return NULL;
}

int turbo_thread_create(turbo_thread_t *thread, turbo_thread_cb entry, void *arg) {
    if (thread == NULL || entry == NULL) return UV_EINVAL;

    struct turbo_thread_wrapper_ctx *ctx = malloc(sizeof(struct turbo_thread_wrapper_ctx));
    if (!ctx) return UV_ENOMEM;
    ctx->entry = entry;
    ctx->arg = arg;

    pthread_t *pt = malloc(sizeof(pthread_t));
    if (!pt) {
        free(ctx);
        return UV_ENOMEM;
    }

    if (pthread_create(pt, NULL, turbo_thread_entry_wrapper_pthread, ctx) != 0) {
        free(ctx);
        free(pt);
        return -1;
    }

    *thread = (turbo_thread_t)pt;
    return 0;
}

int turbo_thread_join(turbo_thread_t *thread) {
    if (thread == NULL || *thread == NULL) return UV_EINVAL;
    pthread_t *pt = (pthread_t *)*thread;
    pthread_join(*pt, NULL);
    free(pt);
    *thread = NULL;
    return 0;
}

void turbo_thread_destroy(turbo_thread_t *thread) {
    if (thread == NULL || *thread == NULL) return;
    pthread_t *pt = (pthread_t *)*thread;
    pthread_detach(*pt);
    free(pt);
    *thread = NULL;
}

void turbo_sleep_ms(uint32_t ms) {
    usleep(ms * 1000);
}

void turbo_thread_yield(void) {
    sched_yield();
}

#endif

// =============================================================================
// Thread Pool (Lock-Free with Disruptor + Object Pool)
// =============================================================================

typedef struct task_node_s {
    turbo_task_fn fn;
    void *arg;
} task_node_t;

typedef struct worker_context_s {
    turbo_threadpool_t *pool;
    disruptor_consumer_t consumer;
    int worker_id;
} worker_context_t;

struct turbo_threadpool_s {
    turbo_thread_t *threads;
    worker_context_t *workers;
    int num_threads;

    disruptor_t *disruptor;
    object_pool_t *task_pool;

    turbo_atomic_int_t shutdown;
    turbo_atomic_int64_t tasks_submitted;
    turbo_atomic_int64_t tasks_completed;

    // Only for wait/destroy synchronization
    turbo_mutex_t wait_mutex;
    turbo_cond_t all_done;
};

static int get_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
#endif
}

static void worker_entry(void *arg) {
    worker_context_t *ctx = (worker_context_t *)arg;
    turbo_threadpool_t *pool = ctx->pool;

    // Register as consumer
    uint64_t next_sequence = disruptor_consumer_register(pool->disruptor, &ctx->consumer);

    // Worker loop
    while (!turbo_atomic_load(&pool->shutdown)) {
        disruptor_cursor_t cursor;
        cursor.sequence = next_sequence;

        // Try non-blocking first to avoid spin
        if (!disruptor_consumer_wait_for_nonblocking(pool->disruptor, &cursor)) {
            turbo_sleep_ms(1);
            continue;
        }

        // Process all available tasks in batch
        for (uint64_t seq = next_sequence; seq <= cursor.sequence; ++seq) {
            disruptor_cursor_t read_cursor;
            read_cursor.sequence = seq;

            // Get task pointer from disruptor
            task_node_t **task_ptr = (task_node_t **)disruptor_show_entry(pool->disruptor, &read_cursor);
            if (!task_ptr || !*task_ptr) continue;

            task_node_t *task = *task_ptr;

            // Execute task
            task->fn(task->arg);

            // Return task to object pool
            object_pool_free(pool->task_pool, task);
            *task_ptr = NULL;

            // Update completion counter
            turbo_atomic_fetch_add64(&pool->tasks_completed, 1);
        }

        // Release entries back to disruptor
        disruptor_consumer_release_entry(pool->disruptor, &ctx->consumer, &cursor);
        next_sequence = cursor.sequence + 1;
    }

    // Final drain on shutdown
    disruptor_cursor_t cursor;
    cursor.sequence = next_sequence;
    if (disruptor_consumer_wait_for_nonblocking(pool->disruptor, &cursor)) {
        for (uint64_t seq = next_sequence; seq <= cursor.sequence; ++seq) {
            disruptor_cursor_t read_cursor;
            read_cursor.sequence = seq;
            task_node_t **task_ptr = (task_node_t **)disruptor_show_entry(pool->disruptor, &read_cursor);
            if (task_ptr && *task_ptr) {
                task_node_t *task = *task_ptr;
                task->fn(task->arg);
                object_pool_free(pool->task_pool, task);
                *task_ptr = NULL;
                turbo_atomic_fetch_add64(&pool->tasks_completed, 1);
            }
        }
        disruptor_consumer_release_entry(pool->disruptor, &ctx->consumer, &cursor);
    }

    disruptor_consumer_unregister(pool->disruptor, &ctx->consumer);
}

turbo_threadpool_t *turbo_threadpool_create(int num_threads) {
    if (num_threads <= 0) {
        num_threads = get_cpu_count();
    }

    turbo_threadpool_t *pool = calloc(1, sizeof(turbo_threadpool_t));
    if (!pool) return NULL;

    pool->num_threads = num_threads;
    turbo_atomic_store(&pool->shutdown, 0);
    turbo_atomic_store64(&pool->tasks_submitted, 0);
    turbo_atomic_store64(&pool->tasks_completed, 0);

    // Create disruptor (MPMC queue)
    disruptor_config_t disruptor_config = {
        .capacity = 4096,  // Power of 2
        .entry_size = sizeof(task_node_t *),
        .consumer_capacity = (uint32_t)num_threads
    };
    pool->disruptor = disruptor_create(&disruptor_config);
    if (!pool->disruptor) {
        free(pool);
        return NULL;
    }

    // Create object pool for tasks
    object_pool_config_t pool_config = {
        .object_size = sizeof(task_node_t),
        .initial_capacity = 1024,
        .max_capacity = 0,  // Unlimited
        .zero_on_alloc = false
    };
    pool->task_pool = object_pool_create(&pool_config);
    if (!pool->task_pool) {
        disruptor_destroy(pool->disruptor);
        free(pool);
        return NULL;
    }

    // Initialize wait synchronization
    turbo_mutex_init(&pool->wait_mutex);
    turbo_cond_init(&pool->all_done);

    // Allocate threads and worker contexts
    pool->threads = calloc(num_threads, sizeof(turbo_thread_t));
    pool->workers = calloc(num_threads, sizeof(worker_context_t));
    if (!pool->threads || !pool->workers) {
        if (pool->threads) free(pool->threads);
        if (pool->workers) free(pool->workers);
        turbo_mutex_destroy(&pool->wait_mutex);
        turbo_cond_destroy(&pool->all_done);
        object_pool_destroy(pool->task_pool);
        disruptor_destroy(pool->disruptor);
        free(pool);
        return NULL;
    }

    // Create worker threads
    for (int i = 0; i < num_threads; i++) {
        pool->workers[i].pool = pool;
        pool->workers[i].worker_id = i;

        if (turbo_thread_create(&pool->threads[i], worker_entry, &pool->workers[i]) != 0) {
            turbo_atomic_store(&pool->shutdown, 1);
            for (int j = 0; j < i; j++) {
                turbo_thread_join(&pool->threads[j]);
            }
            free(pool->threads);
            free(pool->workers);
            turbo_mutex_destroy(&pool->wait_mutex);
            turbo_cond_destroy(&pool->all_done);
            object_pool_destroy(pool->task_pool);
            disruptor_destroy(pool->disruptor);
            free(pool);
            return NULL;
        }
    }

    return pool;
}

void turbo_threadpool_destroy(turbo_threadpool_t *pool) {
    if (!pool) return;

    // Signal shutdown
    turbo_atomic_store(&pool->shutdown, 1);

    // Wait for all worker threads to finish
    for (int i = 0; i < pool->num_threads; i++) {
        turbo_thread_join(&pool->threads[i]);
    }

    // Clean up resources
    turbo_mutex_destroy(&pool->wait_mutex);
    turbo_cond_destroy(&pool->all_done);
    object_pool_destroy(pool->task_pool);
    disruptor_destroy(pool->disruptor);
    free(pool->workers);
    free(pool->threads);
    free(pool);
}

int turbo_threadpool_submit(turbo_threadpool_t *pool, turbo_task_fn task, void *arg) {
    if (!pool || !task) return -1;
    if (turbo_atomic_load(&pool->shutdown)) return -1;

    // Allocate task from object pool (zero malloc)
    task_node_t *node = (task_node_t *)object_pool_alloc(pool->task_pool);
    if (!node) return -1;

    node->fn = task;
    node->arg = arg;

    // Publish to disruptor (lock-free)
    disruptor_cursor_t cursor;
    disruptor_publisher_next_entry_blocking(pool->disruptor, &cursor);

    task_node_t **slot = (task_node_t **)disruptor_acquire_entry(pool->disruptor, &cursor);
    if (!slot) {
        object_pool_free(pool->task_pool, node);
        return -1;
    }

    *slot = node;
    disruptor_publisher_commit_entry_blocking(pool->disruptor, &cursor);

    turbo_atomic_fetch_add64(&pool->tasks_submitted, 1);
    return 0;
}

void turbo_threadpool_wait(turbo_threadpool_t *pool) {
    if (!pool) return;

    // Wait until all submitted tasks are completed
    int64_t submitted = turbo_atomic_load64(&pool->tasks_submitted);
    while (turbo_atomic_load64(&pool->tasks_completed) < submitted) {
        turbo_sleep_ms(1);
    }
}

int turbo_threadpool_pending(turbo_threadpool_t *pool) {
    if (!pool) return 0;

    int64_t submitted = turbo_atomic_load64(&pool->tasks_submitted);
    int64_t completed = turbo_atomic_load64(&pool->tasks_completed);
    return (int)(submitted - completed);
}

int turbo_threadpool_size(turbo_threadpool_t *pool) {
    return pool ? pool->num_threads : 0;
}

// =============================================================================
// Global Synchronization Policy
// =============================================================================

static int g_single_threaded = 0;

void turbo_sync_set_single_threaded(int enabled) {
    g_single_threaded = enabled;
}

int turbo_sync_is_single_threaded(void) {
    return g_single_threaded;
}
