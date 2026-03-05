/**
 * @file real_world_dashboard.c
 * @brief Real-world example: Fetching dashboard data with when_all
 */

#include "netcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Data structures ──────────────────────────────────────── */

typedef struct {
    int id;
    char name[64];
    int is_premium;
} user_t;

typedef struct {
    int count;
    char titles[10][128];
} posts_t;

typedef struct {
    int count;
    char texts[20][256];
} comments_t;

typedef struct {
    user_t user;
    posts_t posts;
    comments_t comments;
} dashboard_t;

/* ── API fetch tasks ──────────────────────────────────────── */

static void fetch_user_api(coro_t *co, void *arg) {
    (void)co;
    user_t *user = (user_t *)arg;
    coro_context_t *ctx = coro_context_current();

    printf("[API] Fetching user data...\n");

    coro_client_t *client = coro_client_create(ctx);
    coro_client_set_timeout(client, 5000);

    int r = coro_client_connect(client, "tcp://api.example.com:80");
    if (r != 0) {
        printf("[API] Failed to connect: %s\n", turbo_strerror(r));
        coro_client_destroy(client);
        return;
    }

    const char *request = "GET /api/user/123 HTTP/1.1\r\nHost: api.example.com\r\n\r\n";
    coro_client_send(client, request, strlen(request));

    char *response;
    size_t len;
    r = coro_client_recv(client, &response, &len);
    if (r == 0) {
        /* Parse response (simplified) */
        user->id = 123;
        snprintf(user->name, sizeof(user->name), "John Doe");
        user->is_premium = 1;
        printf("[API] User fetched: %s (premium: %d)\n", user->name, user->is_premium);
        coro_client_free_recv(response);
    } else {
        printf("[API] Failed to receive: %s\n", turbo_strerror(r));
    }

    coro_client_destroy(client);
}

static void fetch_posts_api(coro_t *co, void *arg) {
    (void)co;
    posts_t *posts = (posts_t *)arg;
    coro_context_t *ctx = coro_context_current();

    printf("[API] Fetching posts...\n");

    coro_client_t *client = coro_client_create(ctx);
    coro_client_set_timeout(client, 5000);

    int r = coro_client_connect(client, "tcp://api.example.com:80");
    if (r != 0) {
        printf("[API] Failed to connect: %s\n", turbo_strerror(r));
        coro_client_destroy(client);
        return;
    }

    const char *request = "GET /api/posts/123 HTTP/1.1\r\nHost: api.example.com\r\n\r\n";
    coro_client_send(client, request, strlen(request));

    char *response;
    size_t len;
    r = coro_client_recv(client, &response, &len);
    if (r == 0) {
        /* Parse response (simplified) */
        posts->count = 3;
        snprintf(posts->titles[0], sizeof(posts->titles[0]), "First Post");
        snprintf(posts->titles[1], sizeof(posts->titles[1]), "Second Post");
        snprintf(posts->titles[2], sizeof(posts->titles[2]), "Third Post");
        printf("[API] Posts fetched: %d posts\n", posts->count);
        coro_client_free_recv(response);
    } else {
        printf("[API] Failed to receive: %s\n", turbo_strerror(r));
    }

    coro_client_destroy(client);
}

static void fetch_comments_api(coro_t *co, void *arg) {
    (void)co;
    comments_t *comments = (comments_t *)arg;
    coro_context_t *ctx = coro_context_current();

    printf("[API] Fetching comments...\n");

    coro_client_t *client = coro_client_create(ctx);
    coro_client_set_timeout(client, 5000);

    int r = coro_client_connect(client, "tcp://api.example.com:80");
    if (r != 0) {
        printf("[API] Failed to connect: %s\n", turbo_strerror(r));
        coro_client_destroy(client);
        return;
    }

    const char *request = "GET /api/comments/123 HTTP/1.1\r\nHost: api.example.com\r\n\r\n";
    coro_client_send(client, request, strlen(request));

    char *response;
    size_t len;
    r = coro_client_recv(client, &response, &len);
    if (r == 0) {
        /* Parse response (simplified) */
        comments->count = 5;
        snprintf(comments->texts[0], sizeof(comments->texts[0]), "Great post!");
        snprintf(comments->texts[1], sizeof(comments->texts[1]), "Thanks for sharing");
        printf("[API] Comments fetched: %d comments\n", comments->count);
        coro_client_free_recv(response);
    } else {
        printf("[API] Failed to receive: %s\n", turbo_strerror(r));
    }

    coro_client_destroy(client);
}

/* ── Main dashboard coroutine ─────────────────────────────── */

static void fetch_dashboard_coro(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = (coro_context_t *)arg;

    printf("=== Dashboard Fetch Example ===\n\n");

    dashboard_t dashboard = {0};

    /* Create lazy tasks for parallel fetching */
    coro_task_t *user_task = coro_task_create(ctx, fetch_user_api, &dashboard.user);
    coro_task_t *posts_task = coro_task_create(ctx, fetch_posts_api, &dashboard.posts);
    coro_task_t *comments_task = coro_task_create(ctx, fetch_comments_api, &dashboard.comments);

    /* Start all tasks concurrently */
    printf("Starting parallel API requests...\n\n");
    coro_task_start(user_task);
    coro_task_start(posts_task);
    coro_task_start(comments_task);

    /* Wait for all to complete */
    printf("Waiting for all API calls to complete...\n\n");
    coro_when_all(ctx, (coro_task_t *[]){user_task, posts_task, comments_task}, 3);

    /* Render dashboard */
    printf("\n=== Dashboard Ready ===\n");
    printf("User: %s (ID: %d, Premium: %s)\n",
           dashboard.user.name,
           dashboard.user.id,
           dashboard.user.is_premium ? "Yes" : "No");

    printf("\nPosts (%d):\n", dashboard.posts.count);
    for (int i = 0; i < dashboard.posts.count; i++) {
        printf("  - %s\n", dashboard.posts.titles[i]);
    }

    printf("\nComments (%d):\n", dashboard.comments.count);
    for (int i = 0; i < dashboard.comments.count && i < 2; i++) {
        printf("  - %s\n", dashboard.comments.texts[i]);
    }
    if (dashboard.comments.count > 2) {
        printf("  ... and %d more\n", dashboard.comments.count - 2);
    }

    printf("\n=== Dashboard Complete ===\n");
}

/* ── Entry point ──────────────────────────────────────────── */

int main(void) {
    coro_context_t *ctx = coro_context_create(NULL);
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    coro_context_spawn(ctx, fetch_dashboard_coro, ctx);
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    return 0;
}
