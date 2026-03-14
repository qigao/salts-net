#include "CoroNet.h"
#include "tinytest.h"
#include "turbo_thread.h"
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define get_thread_id() (unsigned long)GetCurrentThreadId()
#else
#include <pthread.h>
#define get_thread_id() (unsigned long)pthread_self()
#endif

typedef struct {
    unsigned long thread_ids[16];
    int count;
    turbo_mutex_t mutex;
} thread_audit_t;

static void worker_coro(coro_t *co, void *arg) {
    (void)co;
    thread_audit_t *audit = (thread_audit_t *)arg;
    
    unsigned long tid = get_thread_id();
    
    turbo_mutex_lock(&audit->mutex);
    if (audit->count < 16) {
        audit->thread_ids[audit->count++] = tid;
    }
    turbo_mutex_unlock(&audit->mutex);
    
    printf("[Coro] Running on thread %lu\n", tid);
}

spec("coro_thread_pool") {
    describe("Distribution") {
        it("should distribute coroutines across unique threads") {
            coro_thread_pool_t *pool = coro_thread_pool_create(4);
            check_not_null(pool);
            
            thread_audit_t audit = {0};
            turbo_mutex_init(&audit.mutex);
            
            /* Spawn 16 coroutines */
            for (int i = 0; i < 16; i++) {
                coro_thread_pool_spawn(pool, worker_coro, &audit);
            }
            
            /* Wait for them to complete (approximate) */
            turbo_sleep_ms(1000); 
            
            coro_thread_pool_destroy(pool);
            
            /* Audit results */
            turbo_mutex_lock(&audit.mutex);
            printf("[Test] Total executions: %d\n", audit.count);
            
            /* Check if we saw multiple unique thread IDs */
            int unique_threads = 0;
            for (int i = 0; i < audit.count; i++) {
                int found = 0;
                for (int j = 0; j < i; j++) {
                    if (audit.thread_ids[i] == audit.thread_ids[j]) {
                        found = 1;
                        break;
                    }
                }
                if (!found) unique_threads++;
            }
            turbo_mutex_unlock(&audit.mutex);
            
            printf("[Test] Unique threads detected: %d\n", unique_threads);
            check_true(unique_threads > 1);
            
            turbo_mutex_destroy(&audit.mutex);
        }
    }
}
