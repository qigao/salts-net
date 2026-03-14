/**
 * @file coro_pool_example.c
 * @brief Example demonstrating coroutine object pool usage.
 *
 * This example shows how to use the coroutine object pool for high-performance
 * coroutine reuse in high-concurrency scenarios.
 */

#include "CoroNet/turbo_coro_pool.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>

/* ── Example: Manual coroutine pool usage ─────────────────── */

static void worker_task(coro_t *co, void *arg) {
    int task_id = *(int *)arg;
    printf("Task %d: Starting\n", task_id);

    /* Simulate some work */
    for (int i = 0; i < 3; i++) {
        printf("Task %d: Working... (%d/3)\n", task_id, i + 1);
        coro_yield();
    }

    printf("Task %d: Completed\n", task_id);
}

int main(void) {
    printf("=== Coroutine Object Pool Example ===\n\n");

    /* Create a coroutine object pool */
    coro_object_pool_config_t config = {
        .initial_capacity = 4,   /* Pre-allocate 4 coroutines */
        .max_capacity = 16,      /* Allow up to 16 coroutines */
        .stack_size = 0          /* Use default stack size */
    };

    coro_object_pool_t *pool = coro_object_pool_create(&config);
    if (!pool) {
        fprintf(stderr, "Failed to create coroutine pool\n");
        return 1;
    }

    printf("Pool created: %zu free, %zu active, %zu capacity\n",
           coro_object_pool_free_count(pool),
           coro_object_pool_active_count(pool),
           coro_object_pool_capacity(pool));

    /* Spawn multiple tasks using the pool */
    #define NUM_TASKS 8
    coro_t *tasks[NUM_TASKS];
    int task_ids[NUM_TASKS];

    printf("\n--- Acquiring %d coroutines from pool ---\n", NUM_TASKS);
    for (int i = 0; i < NUM_TASKS; i++) {
        task_ids[i] = i + 1;
        tasks[i] = coro_object_pool_acquire(pool, worker_task, &task_ids[i]);
        if (!tasks[i]) {
            fprintf(stderr, "Failed to acquire coroutine %d\n", i);
            continue;
        }
        printf("Acquired task %d (pool: %zu free, %zu active)\n",
               i + 1,
               coro_object_pool_free_count(pool),
               coro_object_pool_active_count(pool));
    }

    /* Run all tasks to completion */
    printf("\n--- Running tasks ---\n");
    int alive_count;
    do {
        alive_count = 0;
        for (int i = 0; i < NUM_TASKS; i++) {
            if (tasks[i] && coro_alive(tasks[i])) {
                coro_resume(tasks[i]);
                alive_count++;
            }
        }
    } while (alive_count > 0);

    /* Release coroutines back to pool */
    printf("\n--- Releasing coroutines back to pool ---\n");
    for (int i = 0; i < NUM_TASKS; i++) {
        if (tasks[i]) {
            coro_object_pool_release(pool, tasks[i]);
            printf("Released task %d (pool: %zu free, %zu active)\n",
                   i + 1,
                   coro_object_pool_free_count(pool),
                   coro_object_pool_active_count(pool));
        }
    }

    /* Destroy pool */
    printf("\n--- Destroying pool ---\n");
    coro_object_pool_destroy(pool);
    printf("Pool destroyed\n");

    printf("\n=== Example completed successfully ===\n");
    return 0;
}
