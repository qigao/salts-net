/**
 * @file when_any_example.c
 * @brief Example: Implementing when_any combinator (race condition)
 */

#include "netcore.h" 
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>

/* ── Example tasks (different speeds) ─────────────────────── */

static void fast_task(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = coro_context_current();

    printf("[Fast] Starting...\n");
    coro_sleep(ctx, 50);
    printf("[Fast] Completed!\n");

    *(const char **)arg = "Fast task won!";
}

static void medium_task(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = coro_context_current();

    printf("[Medium] Starting...\n");
    coro_sleep(ctx, 100);
    printf("[Medium] Completed!\n");

    *(const char **)arg = "Medium task won!";
}

static void slow_task(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = coro_context_current();

    printf("[Slow] Starting...\n");
    coro_sleep(ctx, 200);
    printf("[Slow] Completed!\n");

    *(const char **)arg = "Slow task won!";
}

/* ── Main coroutine ───────────────────────────────────────── */

static void main_coro(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = (coro_context_t *)arg;

    printf("=== when_any Example (Race) ===\n\n");

    /* Results storage */
    const char *result1 = NULL;
    const char *result2 = NULL;
    const char *result3 = NULL;

    /* Create lazy tasks */
    coro_task_t *task1 = coro_task_create(ctx, fast_task, &result1);
    coro_task_t *task2 = coro_task_create(ctx, medium_task, &result2);
    coro_task_t *task3 = coro_task_create(ctx, slow_task, &result3);

    /* Start all tasks concurrently */
    printf("Starting race...\n");
    coro_task_start(task1);
    coro_task_start(task2);
    coro_task_start(task3);

    /* Wait for first to complete */
    printf("Waiting for first task to complete...\n\n");
    int winner = coro_when_any(ctx, (coro_task_t *[]){task1, task2, task3}, 3);

    printf("\n=== Race finished! ===\n");
    printf("Winner: Task %d\n", winner + 1);

    const char *results[] = {result1, result2, result3};
    if (results[winner]) {
        printf("Result: %s\n", results[winner]);
    }

    /* Note: Other tasks continue running in background */
    printf("\nNote: Other tasks continue running...\n");
    coro_sleep(ctx, 200);  /* Wait for others to finish */
    printf("All tasks completed.\n");
}

/* ── Entry point ──────────────────────────────────────────── */

int main(void) {
    coro_context_t *ctx = coro_context_create(NULL);
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    coro_context_spawn(ctx, main_coro, ctx);
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    return 0;
}
