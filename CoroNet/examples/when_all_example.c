/**
 * @file when_all_example.c
 * @brief Example: Implementing when_all combinator for lazy tasks
 */

#include "CoroNet.h" 
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>

/* ── Example tasks ────────────────────────────────────────── */

static void fetch_user(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = coro_context_current();

    printf("[Task 1] Fetching user...\n");
    coro_sleep(ctx, 100);  /* Simulate network delay */
    printf("[Task 1] User fetched!\n");

    *(int *)arg = 123;  /* Store result */
}

static void fetch_posts(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = coro_context_current();

    printf("[Task 2] Fetching posts...\n");
    coro_sleep(ctx, 150);  /* Simulate network delay */
    printf("[Task 2] Posts fetched!\n");

    *(int *)arg = 456;  /* Store result */
}

static void fetch_comments(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = coro_context_current();

    printf("[Task 3] Fetching comments...\n");
    coro_sleep(ctx, 80);  /* Simulate network delay */
    printf("[Task 3] Comments fetched!\n");

    *(int *)arg = 789;  /* Store result */
}

/* ── Main coroutine ───────────────────────────────────────── */

static void main_coro(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = (coro_context_t *)arg;

    printf("=== when_all Example ===\n\n");

    /* Results storage */
    int user_id = 0;
    int posts_count = 0;
    int comments_count = 0;

    /* Create lazy tasks */
    coro_task_t *task1 = coro_task_create(ctx, fetch_user, &user_id);
    coro_task_t *task2 = coro_task_create(ctx, fetch_posts, &posts_count);
    coro_task_t *task3 = coro_task_create(ctx, fetch_comments, &comments_count);

    printf("Tasks created (not started yet)\n\n");

    /* Start all tasks concurrently */
    printf("Starting all tasks...\n");
    coro_task_start(task1);
    coro_task_start(task2);
    coro_task_start(task3);

    /* Wait for all to complete */
    printf("Waiting for all tasks to complete...\n\n");
    coro_when_all(ctx, (coro_task_t *[]){task1, task2, task3}, 3);

    printf("\n=== All tasks completed! ===\n");
    printf("User ID: %d\n", user_id);
    printf("Posts count: %d\n", posts_count);
    printf("Comments count: %d\n", comments_count);
}

/* ── Entry point ──────────────────────────────────────────── */

int main(void) {
    coro_context_t *ctx = coro_context_create(NULL);
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    /* Spawn main coroutine */
    coro_context_spawn(ctx, main_coro, ctx);

    /* Run event loop */
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    return 0;
}
