/**
 * @file coro_pool_example.c
 * @brief High-frequency coroutine pool with the scheduler API.
 *
 * Shows the recommended pattern for launching many short-lived coroutines
 * without paying the full create/destroy cost on every task:
 *
 *   coro_spawn_pooled(sched, pool, fn, arg)
 *
 * The pool pre-allocates coroutine shells; each spawn borrows one, runs it,
 * and returns it automatically on completion.
 *
 * Compare with coro_context_spawn(), which integrates with an I/O event loop.
 * coro_spawn_pooled() targets pure-CPU workloads with a dedicated scheduler.
 *
 * Usage: ./coro_pool_example
 */

#include "CoroNet.h"
#include <stdio.h>
#include <stdlib.h>

#define NUM_TASKS 8

/* ── Worker coroutine ─────────────────────────────────────── */

/**
 * @brief Increment a shared counter.
 *
 * Yields once to demonstrate cooperative scheduling between pool tasks.
 */
static void worker(coro_t *co, void *arg) {
    UNUSED(co);
    int *counter = (int *)arg;
    int id = *counter + 1;  /* snapshot before yield races */

    printf("[Worker %d] Starting\n", id);
    coro_yield();            /* give other pool tasks a turn */
    printf("[Worker %d] Resuming and completing\n", id);

    (*counter)++;
}

/* ── Entry point ──────────────────────────────────────────── */

int main(void) {
    printf("=== Coroutine Pool Example ===\n\n");

    coro_context_t *ctx = coro_context_create(NULL);

    /* Scheduler: owns the run loop for pure-compute tasks. */
    coro_scheduler_t *sched = coro_scheduler_create();

    /* Pool: pre-allocate coroutine shells to amortise alloc cost. */
    coro_object_pool_config_t cfg = CORO_OBJECT_POOL_CONFIG_DEFAULT;
    cfg.initial_capacity = 4;
    cfg.max_capacity     = NUM_TASKS;
    coro_object_pool_t *pool = coro_object_pool_create(&cfg, ctx);

    int counters[NUM_TASKS];
    for (int i = 0; i < NUM_TASKS; i++) {
        counters[i] = i;  /* pass distinct values so workers are identifiable */
        coro_spawn_pooled(sched, pool, worker, &counters[i]);
        printf("[Main] Spawned task %d (pool capacity %zu, active %zu)\n",
               i + 1,
               coro_object_pool_capacity(pool),
               coro_object_pool_active_count(pool));
    }

    printf("\n--- Running all tasks ---\n");
    coro_scheduler_run(sched);  /* blocks until every task completes */
    printf("--- All tasks done ---\n\n");

    printf("Active pool slots after run: %zu (should be 0)\n",
           coro_object_pool_active_count(pool));

    coro_object_pool_destroy(pool);
    coro_scheduler_destroy(sched);
    coro_context_destroy(ctx);

    printf("\n=== Example completed ===\n");
    return 0;
}
