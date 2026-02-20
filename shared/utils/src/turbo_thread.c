/**
 * @file turbo_thread.c
 * @brief Threading primitives and thread pool implementation
 *
 * Cross-platform: Windows SRW Lock + Condition Variable, POSIX pthread.
 */

#include "turbo_thread.h"
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
    if (mutex == NULL || *mutex == NULL || *mutex == (turbo_mutex_t)(uintptr_t)-1) return;
    free(*mutex);
    *mutex = NULL;
}

void turbo_mutex_lock(turbo_mutex_t *mutex) {
    if (mutex == NULL || *mutex == NULL || *mutex == (turbo_mutex_t)(uintptr_t)-1) return;
    AcquireSRWLockExclusive((PSRWLOCK)*mutex);
}

void turbo_mutex_unlock(turbo_mutex_t *mutex) {
    if (mutex == NULL || *mutex == NULL || *mutex == (turbo_mutex_t)(uintptr_t)-1) return;
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
    if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL ||
        *mutex == (turbo_mutex_t)(uintptr_t)-1) return;
    SleepConditionVariableSRW((PCONDITION_VARIABLE)*cond, (PSRWLOCK)*mutex, INFINITE, 0);
}

int turbo_cond_timedwait(turbo_cond_t *cond, turbo_mutex_t *mutex, uint64_t timeout_ns) {
    if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL ||
        *mutex == (turbo_mutex_t)(uintptr_t)-1) return UV_EINVAL;
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
// Thread Pool (platform-independent)
// =============================================================================

typedef struct task_node_s {
    turbo_task_fn fn;
    void *arg;
    struct task_node_s *next;
} task_node_t;

struct turbo_threadpool_s {
    turbo_thread_t *threads;
    int num_threads;

    task_node_t *head;
    task_node_t *tail;
    int pending;
    int active;

    turbo_mutex_t lock;
    turbo_cond_t not_empty;
    turbo_cond_t all_done;

    int shutdown;
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
    turbo_threadpool_t *pool = (turbo_threadpool_t *)arg;

    for (;;) {
        turbo_mutex_lock(&pool->lock);

        while (pool->head == NULL && !pool->shutdown) {
            turbo_cond_wait(&pool->not_empty, &pool->lock);
        }

        if (pool->shutdown && pool->head == NULL) {
            turbo_mutex_unlock(&pool->lock);
            break;
        }

        task_node_t *node = pool->head;
        pool->head = node->next;
        if (pool->head == NULL) {
            pool->tail = NULL;
        }
        pool->pending--;
        pool->active++;

        turbo_mutex_unlock(&pool->lock);

        node->fn(node->arg);
        free(node);

        turbo_mutex_lock(&pool->lock);
        pool->active--;
        if (pool->pending == 0 && pool->active == 0) {
            turbo_cond_broadcast(&pool->all_done);
        }
        turbo_mutex_unlock(&pool->lock);
    }
}

turbo_threadpool_t *turbo_threadpool_create(int num_threads) {
    if (num_threads <= 0) {
        num_threads = get_cpu_count();
    }

    turbo_threadpool_t *pool = calloc(1, sizeof(turbo_threadpool_t));
    if (!pool) return NULL;

    pool->threads = calloc(num_threads, sizeof(turbo_thread_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }

    pool->num_threads = num_threads;
    turbo_mutex_init(&pool->lock);
    turbo_cond_init(&pool->not_empty);
    turbo_cond_init(&pool->all_done);

    for (int i = 0; i < num_threads; i++) {
        if (turbo_thread_create(&pool->threads[i], worker_entry, pool) != 0) {
            pool->shutdown = 1;
            turbo_cond_broadcast(&pool->not_empty);
            for (int j = 0; j < i; j++) {
                turbo_thread_join(&pool->threads[j]);
            }
            turbo_mutex_destroy(&pool->lock);
            turbo_cond_destroy(&pool->not_empty);
            turbo_cond_destroy(&pool->all_done);
            free(pool->threads);
            free(pool);
            return NULL;
        }
    }

    return pool;
}

void turbo_threadpool_destroy(turbo_threadpool_t *pool) {
    if (!pool) return;

    turbo_mutex_lock(&pool->lock);
    pool->shutdown = 1;
    turbo_cond_broadcast(&pool->not_empty);
    turbo_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->num_threads; i++) {
        turbo_thread_join(&pool->threads[i]);
    }

    task_node_t *node = pool->head;
    while (node) {
        task_node_t *next = node->next;
        free(node);
        node = next;
    }

    turbo_mutex_destroy(&pool->lock);
    turbo_cond_destroy(&pool->not_empty);
    turbo_cond_destroy(&pool->all_done);
    free(pool->threads);
    free(pool);
}

int turbo_threadpool_submit(turbo_threadpool_t *pool, turbo_task_fn task, void *arg) {
    if (!pool || !task) return -1;

    task_node_t *node = malloc(sizeof(task_node_t));
    if (!node) return -1;

    node->fn = task;
    node->arg = arg;
    node->next = NULL;

    turbo_mutex_lock(&pool->lock);

    if (pool->shutdown) {
        turbo_mutex_unlock(&pool->lock);
        free(node);
        return -1;
    }

    if (pool->tail) {
        pool->tail->next = node;
    } else {
        pool->head = node;
    }
    pool->tail = node;
    pool->pending++;

    turbo_cond_signal(&pool->not_empty);
    turbo_mutex_unlock(&pool->lock);

    return 0;
}

void turbo_threadpool_wait(turbo_threadpool_t *pool) {
    if (!pool) return;

    turbo_mutex_lock(&pool->lock);
    while (pool->pending > 0 || pool->active > 0) {
        turbo_cond_wait(&pool->all_done, &pool->lock);
    }
    turbo_mutex_unlock(&pool->lock);
}

int turbo_threadpool_pending(turbo_threadpool_t *pool) {
    if (!pool) return 0;

    turbo_mutex_lock(&pool->lock);
    int count = pool->pending + pool->active;
    turbo_mutex_unlock(&pool->lock);

    return count;
}

int turbo_threadpool_size(turbo_threadpool_t *pool) {
    return pool ? pool->num_threads : 0;
}
