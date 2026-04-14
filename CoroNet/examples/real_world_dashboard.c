/**
 * @file real_world_dashboard.c
 * @brief Multi-task concurrent fetch using when_all.
 *
 * Models three parallel "API calls" (simulated with coro_sleep) feeding into
 * a single dashboard render. The key insight: coro_when_all() suspends the
 * caller until every task completes while the event loop continues running
 * other coroutines — total wall time equals max(task latencies), not their sum.
 *
 * Pattern:
 *   1. coro_task_create()  — define task, do NOT start yet.
 *   2. coro_task_start()   — schedule all tasks concurrently.
 *   3. coro_when_all()     — suspend caller until all are done.
 *
 * Usage: ./real_world_dashboard
 */

#include "CoroNet.h"
#include <stdio.h>
#include <stdlib.h>

/* ── Data structures ──────────────────────────────────────── */

typedef struct {
    int  id;
    char name[64];
    int  is_premium;
} user_t;

typedef struct {
    int  count;
    char titles[3][64];
} posts_t;

typedef struct {
    int  count;
    char preview[64];  /* first comment text */
} comments_t;

typedef struct {
    user_t     user;
    posts_t    posts;
    comments_t comments;
} dashboard_t;

/* ── Simulated API tasks ──────────────────────────────────── */

/**
 * @brief Simulate fetching user profile (~80 ms network round-trip).
 */
static void fetch_user(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = coro_context_current();
    user_t *out = (user_t *)arg;

    printf("[User]     Fetching...\n");
    coro_sleep(ctx, 80);

    out->id         = 42;
    out->is_premium = 1;
    snprintf(out->name, sizeof(out->name), "Ada Lovelace");
    printf("[User]     Done.\n");
}

/**
 * @brief Simulate fetching recent posts (~120 ms network round-trip).
 */
static void fetch_posts(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = coro_context_current();
    posts_t *out = (posts_t *)arg;

    printf("[Posts]    Fetching...\n");
    coro_sleep(ctx, 120);

    out->count = 3;
    snprintf(out->titles[0], sizeof(out->titles[0]), "Notes on the Analytical Engine");
    snprintf(out->titles[1], sizeof(out->titles[1]), "On Bernoulli Numbers");
    snprintf(out->titles[2], sizeof(out->titles[2]), "Translator's Notes");
    printf("[Posts]    Done.\n");
}

/**
 * @brief Simulate fetching comments (~60 ms network round-trip).
 */
static void fetch_comments(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = coro_context_current();
    comments_t *out = (comments_t *)arg;

    printf("[Comments] Fetching...\n");
    coro_sleep(ctx, 60);

    out->count = 5;
    snprintf(out->preview, sizeof(out->preview), "Brilliant work, as always.");
    printf("[Comments] Done.\n");
}

/* ── Dashboard orchestrator ───────────────────────────────── */

/**
 * @brief Spawn all fetches concurrently, wait for all, then render.
 *
 * Wall time = max(80, 120, 60) ms = 120 ms — not 80+120+60 = 260 ms.
 */
static void fetch_dashboard(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = (coro_context_t *)arg;
    dashboard_t db = {0};

    printf("=== Starting parallel fetch ===\n\n");

    coro_task_t *t_user     = coro_task_create(ctx, fetch_user,     &db.user);
    coro_task_t *t_posts    = coro_task_create(ctx, fetch_posts,    &db.posts);
    coro_task_t *t_comments = coro_task_create(ctx, fetch_comments, &db.comments);

    /* Start all three concurrently, then suspend until all finish. */
    coro_task_start(t_user);
    coro_task_start(t_posts);
    coro_task_start(t_comments);

    coro_when_all(ctx, (coro_task_t *[]){t_user, t_posts, t_comments}, 3);

    /* All tasks done: render. */
    printf("\n=== Dashboard ===\n");
    printf("User   : %s (id=%d, premium=%s)\n",
           db.user.name, db.user.id, db.user.is_premium ? "yes" : "no");
    printf("Posts  : %d total\n", db.posts.count);
    for (int i = 0; i < db.posts.count; i++)
        printf("         - %s\n", db.posts.titles[i]);
    printf("Comments: %d total — \"%s\"\n", db.comments.count, db.comments.preview);
}

/* ── Entry point ──────────────────────────────────────────── */

int main(void) {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_context_spawn(ctx, fetch_dashboard, ctx);
    coro_context_run(ctx, TURBO_RUN_DEFAULT);
    coro_context_destroy(ctx);
    return 0;
}
