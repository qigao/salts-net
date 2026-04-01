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
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <process.h>
  #include <windows.h>
#else
  #include <errno.h>
  #include <pthread.h>
  #include <sched.h>
  #include <time.h>
  #include <unistd.h>
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
  BOOL result =
      SleepConditionVariableSRW((PCONDITION_VARIABLE)*cond, (PSRWLOCK)*mutex, timeout_ms, 0);
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

void turbo_sleep_ms(uint32_t ms) { Sleep(ms); }

void turbo_thread_yield(void) { SwitchToThread(); }

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

void turbo_once(turbo_once_t *guard, void (*callback)(void)) { pthread_once(guard, callback); }

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

void turbo_sleep_ms(uint32_t ms) { usleep(ms * 1000); }

void turbo_thread_yield(void) { sched_yield(); }

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
  size_t queue_capacity;
  size_t ring_capacity;

  disruptor_t *disruptor;
  object_pool_t *task_pool;

  atomic_int accepting;
  atomic_int shutdown;
  _Atomic int64_t queued_depth;
  _Atomic int64_t tasks_submitted;
  _Atomic int64_t tasks_started;
  _Atomic int64_t tasks_completed;
  _Atomic int64_t tasks_rejected;

  turbo_mutex_t wait_mutex;
  turbo_cond_t all_done;
};

#define TURBO_THREADPOOL_DEFAULT_QUEUE_CAPACITY 4096U
#define TURBO_THREADPOOL_MIN_RING_CAPACITY 64U

static uint64_t turbo_threadpool_round_up_pow2(size_t value) {
  uint64_t rounded = 1U;

  if (value == 0U) {
    return 0U;
  }

  while (rounded < (uint64_t)value) {
    if (rounded > (UINT64_MAX >> 1U)) {
      return 0U;
    }
    rounded <<= 1U;
  }

  return rounded;
}

static int64_t turbo_threadpool_pending_tasks(const turbo_threadpool_t *pool) {
  int64_t submitted;
  int64_t completed;

  if (pool == NULL) {
    return 0;
  }

  submitted = atomic_load(&pool->tasks_submitted);
  completed = atomic_load(&pool->tasks_completed);
  return submitted - completed;
}

static int turbo_threadpool_try_reserve_queue_slot(turbo_threadpool_t *pool) {
  int64_t current_depth;

  if (pool == NULL) {
    return 0;
  }

  current_depth = atomic_load(&pool->queued_depth);
  while (current_depth < (int64_t)pool->queue_capacity) {
    if (atomic_compare_exchange_weak(&pool->queued_depth, &current_depth, current_depth + 1)) {
      return 1;
    }
  }

  return 0;
}

static void turbo_threadpool_release_queue_slot(turbo_threadpool_t *pool) {
  if (pool == NULL) {
    return;
  }

  atomic_fetch_sub(&pool->queued_depth, 1);
}

static void turbo_threadpool_notify_progress(turbo_threadpool_t *pool) {
  if (pool == NULL) {
    return;
  }

  turbo_mutex_lock(&pool->wait_mutex);
  if (turbo_threadpool_pending_tasks(pool) <= 0) {
    turbo_cond_broadcast(&pool->all_done);
  }
  turbo_mutex_unlock(&pool->wait_mutex);
}

static void turbo_threadpool_finish_task(turbo_threadpool_t *pool) {
  atomic_fetch_add(&pool->tasks_completed, 1);
  turbo_threadpool_notify_progress(pool);
}

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
  while (!atomic_load(&pool->shutdown)) {
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

      turbo_threadpool_release_queue_slot(pool);
      atomic_fetch_add(&pool->tasks_started, 1);
      task->fn(task->arg);
      object_pool_free(pool->task_pool, task);
      *task_ptr = NULL;
      turbo_threadpool_finish_task(pool);
    }

    disruptor_consumer_release_entry(pool->disruptor, &ctx->consumer, &cursor);
    next_sequence = cursor.sequence + 1;
  }

  disruptor_cursor_t cursor;
  cursor.sequence = next_sequence;
  if (disruptor_consumer_wait_for_nonblocking(pool->disruptor, &cursor)) {
    for (uint64_t seq = next_sequence; seq <= cursor.sequence; ++seq) {
      disruptor_cursor_t read_cursor;
      read_cursor.sequence = seq;
      task_node_t **task_ptr = (task_node_t **)disruptor_show_entry(pool->disruptor, &read_cursor);
      if (task_ptr && *task_ptr) {
        task_node_t *task = *task_ptr;
        turbo_threadpool_release_queue_slot(pool);
        atomic_fetch_add(&pool->tasks_started, 1);
        task->fn(task->arg);
        object_pool_free(pool->task_pool, task);
        *task_ptr = NULL;
        turbo_threadpool_finish_task(pool);
      }
    }
    disruptor_consumer_release_entry(pool->disruptor, &ctx->consumer, &cursor);
  }

  disruptor_consumer_unregister(pool->disruptor, &ctx->consumer);
  turbo_threadpool_notify_progress(pool);
}

turbo_threadpool_t *turbo_threadpool_create_with_config(const turbo_threadpool_config_t *config) {
  turbo_threadpool_t *pool;
  object_pool_config_t pool_config;
  disruptor_config_t disruptor_config;
  uint64_t ring_capacity;
  int num_threads;
  size_t queue_capacity;

  if (config == NULL) {
    return NULL;
  }

  num_threads = config->num_threads;
  if (num_threads <= 0) {
    num_threads = get_cpu_count();
  }
  queue_capacity =
      config->queue_capacity > 0U ? config->queue_capacity : TURBO_THREADPOOL_DEFAULT_QUEUE_CAPACITY;
  ring_capacity = turbo_threadpool_round_up_pow2(queue_capacity);
  if (ring_capacity == 0U || ring_capacity > (uint64_t)SIZE_MAX) {
    return NULL;
  }
  if (ring_capacity < TURBO_THREADPOOL_MIN_RING_CAPACITY) {
    ring_capacity = TURBO_THREADPOOL_MIN_RING_CAPACITY;
  }

  pool = calloc(1, sizeof(turbo_threadpool_t));
  if (!pool) return NULL;

  pool->num_threads = num_threads;
  pool->queue_capacity = queue_capacity;
  pool->ring_capacity = (size_t)ring_capacity;
  atomic_store(&pool->accepting, 1);
  atomic_store(&pool->shutdown, 0);
  atomic_store(&pool->queued_depth, 0);
  atomic_store(&pool->tasks_submitted, 0);
  atomic_store(&pool->tasks_started, 0);
  atomic_store(&pool->tasks_completed, 0);
  atomic_store(&pool->tasks_rejected, 0);

  disruptor_config.entry_size = sizeof(task_node_t *);
  disruptor_config.capacity = ring_capacity;
  disruptor_config.consumer_capacity = (uint32_t)num_threads;
  pool->disruptor = disruptor_create(&disruptor_config);
  if (!pool->disruptor) {
    free(pool);
    return NULL;
  }

  pool_config.object_size = sizeof(task_node_t);
  pool_config.initial_capacity = queue_capacity < 1024U ? queue_capacity : 1024U;
  pool_config.max_capacity = 0;
  pool_config.zero_on_alloc = false;
  pool->task_pool = object_pool_create(&pool_config);
  if (!pool->task_pool) {
    disruptor_destroy(pool->disruptor);
    free(pool);
    return NULL;
  }

  turbo_mutex_init(&pool->wait_mutex);
  turbo_cond_init(&pool->all_done);

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

  for (int i = 0; i < num_threads; i++) {
    pool->workers[i].pool = pool;
    pool->workers[i].worker_id = i;

    if (turbo_thread_create(&pool->threads[i], worker_entry, &pool->workers[i]) != 0) {
      atomic_store(&pool->accepting, 0);
      atomic_store(&pool->shutdown, 1);
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

turbo_threadpool_t *turbo_threadpool_create(int num_threads) {
  turbo_threadpool_config_t config;

  config.num_threads = num_threads;
  config.queue_capacity = TURBO_THREADPOOL_DEFAULT_QUEUE_CAPACITY;
  return turbo_threadpool_create_with_config(&config);
}

void turbo_threadpool_shutdown(turbo_threadpool_t *pool) {
  if (!pool) return;

  atomic_store(&pool->accepting, 0);
  atomic_store(&pool->shutdown, 1);
  turbo_threadpool_notify_progress(pool);
}

void turbo_threadpool_destroy(turbo_threadpool_t *pool) {
  if (!pool) return;

  turbo_threadpool_shutdown(pool);
  for (int i = 0; i < pool->num_threads; i++) {
    turbo_thread_join(&pool->threads[i]);
  }

  turbo_mutex_destroy(&pool->wait_mutex);
  turbo_cond_destroy(&pool->all_done);
  object_pool_destroy(pool->task_pool);
  disruptor_destroy(pool->disruptor);
  free(pool->workers);
  free(pool->threads);
  free(pool);
}

static int turbo_threadpool_submit_internal(turbo_threadpool_t *pool, turbo_task_fn task, void *arg,
                                            int blocking) {
  disruptor_cursor_t cursor;
  task_node_t **slot;
  task_node_t *node;
  unsigned int wait_rounds = 0U;

  if (!pool || !task) return -1;
  if (!atomic_load(&pool->accepting) || atomic_load(&pool->shutdown)) {
    atomic_fetch_add(&pool->tasks_rejected, 1);
    return -1;
  }

  node = (task_node_t *)object_pool_alloc(pool->task_pool);
  if (!node) {
    atomic_fetch_add(&pool->tasks_rejected, 1);
    return -1;
  }

  node->fn = task;
  node->arg = arg;

  while (atomic_load(&pool->accepting) && !atomic_load(&pool->shutdown)) {
    if (!turbo_threadpool_try_reserve_queue_slot(pool)) {
      if (!blocking) {
        object_pool_free(pool->task_pool, node);
        atomic_fetch_add(&pool->tasks_rejected, 1);
        return -1;
      }

      wait_rounds++;
      if ((wait_rounds & 0xFFU) == 0U) {
        turbo_sleep_ms(1);
      } else {
        turbo_thread_yield();
      }
      continue;
    }

    if (disruptor_publisher_try_claim(pool->disruptor, &cursor)) {
      slot = (task_node_t **)disruptor_acquire_entry(pool->disruptor, &cursor);
      if (!slot) {
        turbo_threadpool_release_queue_slot(pool);
        break;
      }

      *slot = node;
      disruptor_publisher_commit_entry_blocking(pool->disruptor, &cursor);
      atomic_fetch_add(&pool->tasks_submitted, 1);
      return 0;
    }

    turbo_threadpool_release_queue_slot(pool);

    wait_rounds++;
    if ((wait_rounds & 0xFFU) == 0U) {
      turbo_sleep_ms(1);
    } else {
      turbo_thread_yield();
    }
  }

  object_pool_free(pool->task_pool, node);
  atomic_fetch_add(&pool->tasks_rejected, 1);
  return -1;
}

int turbo_threadpool_submit(turbo_threadpool_t *pool, turbo_task_fn task, void *arg) {
  return turbo_threadpool_submit_internal(pool, task, arg, 1);
}

int turbo_threadpool_try_submit(turbo_threadpool_t *pool, turbo_task_fn task, void *arg) {
  return turbo_threadpool_submit_internal(pool, task, arg, 0);
}

void turbo_threadpool_wait(turbo_threadpool_t *pool) {
  if (!pool) return;

  turbo_mutex_lock(&pool->wait_mutex);
  while (turbo_threadpool_pending_tasks(pool) > 0) {
    turbo_cond_wait(&pool->all_done, &pool->wait_mutex);
  }
  turbo_mutex_unlock(&pool->wait_mutex);
}

int turbo_threadpool_pending(turbo_threadpool_t *pool) {
  return (int)turbo_threadpool_pending_tasks(pool);
}

int turbo_threadpool_size(turbo_threadpool_t *pool) { return pool ? pool->num_threads : 0; }

size_t turbo_threadpool_capacity(turbo_threadpool_t *pool) {
  return pool ? pool->queue_capacity : 0U;
}

int turbo_threadpool_is_accepting(turbo_threadpool_t *pool) {
  return pool ? atomic_load(&pool->accepting) : 0;
}

void turbo_threadpool_get_stats(turbo_threadpool_t *pool, turbo_threadpool_stats_t *stats) {
  int64_t submitted;
  int64_t started;
  int64_t completed;

  if (pool == NULL || stats == NULL) {
    return;
  }

  memset(stats, 0, sizeof(*stats));

  submitted = atomic_load(&pool->tasks_submitted);
  started = atomic_load(&pool->tasks_started);
  completed = atomic_load(&pool->tasks_completed);

  stats->num_threads = pool->num_threads;
  stats->queue_capacity = pool->queue_capacity;
  stats->accepting = atomic_load(&pool->accepting);
  stats->submitted_tasks = submitted;
  stats->started_tasks = started;
  stats->completed_tasks = completed;
  stats->rejected_tasks = atomic_load(&pool->tasks_rejected);
  stats->queued_tasks = atomic_load(&pool->queued_depth);
  stats->active_tasks = started - completed;
  stats->pending_tasks = submitted - completed;
}

// =============================================================================
// Global Synchronization Policy
// =============================================================================

static int g_single_threaded = 0;

void turbo_sync_set_single_threaded(int enabled) { g_single_threaded = enabled; }

int turbo_sync_is_single_threaded(void) { return g_single_threaded; }

// =============================================================================
// Read-Write Lock - cross-platform rwlock abstraction
// =============================================================================

#ifdef _WIN32

int turbo_rwlock_init(turbo_rwlock_t *lock) {
  if (!lock) return -1;
  InitializeSRWLock(&lock->lock);
  return 0;
}

void turbo_rwlock_destroy(turbo_rwlock_t *lock) { (void)lock; /* SRWLOCK needs no cleanup */ }

void turbo_rwlock_rdlock(turbo_rwlock_t *lock) { AcquireSRWLockShared(&lock->lock); }

void turbo_rwlock_rdunlock(turbo_rwlock_t *lock) { ReleaseSRWLockShared(&lock->lock); }

void turbo_rwlock_wrlock(turbo_rwlock_t *lock) { AcquireSRWLockExclusive(&lock->lock); }

void turbo_rwlock_wrunlock(turbo_rwlock_t *lock) { ReleaseSRWLockExclusive(&lock->lock); }

#else

int turbo_rwlock_init(turbo_rwlock_t *lock) {
  if (!lock) return -1;
  return pthread_rwlock_init(&lock->lock, NULL);
}

void turbo_rwlock_destroy(turbo_rwlock_t *lock) {
  if (lock) pthread_rwlock_destroy(&lock->lock);
}

void turbo_rwlock_rdlock(turbo_rwlock_t *lock) { pthread_rwlock_rdlock(&lock->lock); }

void turbo_rwlock_rdunlock(turbo_rwlock_t *lock) { pthread_rwlock_unlock(&lock->lock); }

void turbo_rwlock_wrlock(turbo_rwlock_t *lock) { pthread_rwlock_wrlock(&lock->lock); }

void turbo_rwlock_wrunlock(turbo_rwlock_t *lock) { pthread_rwlock_unlock(&lock->lock); }

#endif

int turbo_getpid(void) {
#ifdef _WIN32
  return (int)GetCurrentProcessId();
#else
  return (int)getpid();
#endif
}
