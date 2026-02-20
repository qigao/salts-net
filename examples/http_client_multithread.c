/**
 * @file http_client_multithread.c
 * @brief Multi-threaded HTTP client example using TurboNet thread pool.
 *
 * Demonstrates:
 * - Thread pool for parallel HTTP requests
 * - Thread-safe HTTP clients (one per task)
 * - Simple task submission and wait
 */

#include "http_client.h"
#include "turbo_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define UNUSED(x) (void)(x)

// Task definition
typedef struct {
    int id;
    const char* url;
    int status_code;
    size_t body_len;
    char error[256];
} http_task_t;

// Shared state
static turbo_mutex_t g_print_mutex;
static int g_completed = 0;
static int g_success = 0;

static void safe_print(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    turbo_mutex_lock(&g_print_mutex);
    vprintf(fmt, args);
    fflush(stdout);
    turbo_mutex_unlock(&g_print_mutex);
    va_end(args);
}

// Worker task function
static void fetch_url(void* arg) {
    http_task_t* task = (http_task_t*)arg;

    // Each task creates its own client (thread-safe)
    http_client_t* client = http_client_create();
    if (!client) {
        snprintf(task->error, sizeof(task->error), "Failed to create HTTP client");
        return;
    }

    http_client_set_timeout(client, 10000);
    http_client_set_user_agent(client, "TurboNet-ThreadPool/1.0");

    safe_print("[Task %d] Fetching: %s\n", task->id, task->url);

    http_response_t* response = http_get(client, task->url);

    if (response) {
        task->status_code = response->status_code;
        task->body_len = response->body_len;

        if (response->error) {
            snprintf(task->error, sizeof(task->error), "%s", response->error);
        } else {
            task->error[0] = '\0';
            turbo_mutex_lock(&g_print_mutex);
            g_success++;
            turbo_mutex_unlock(&g_print_mutex);
        }

        http_response_free(response);
    } else {
        snprintf(task->error, sizeof(task->error), "No response");
    }

    http_client_destroy(client);

    turbo_mutex_lock(&g_print_mutex);
    g_completed++;
    turbo_mutex_unlock(&g_print_mutex);

    safe_print("[Task %d] Done: status=%d, size=%zu bytes\n",
               task->id, task->status_code, task->body_len);
}

int main(int argc, char* argv[]) {
    UNUSED(argc);
    UNUSED(argv);

    const char* urls[] = {
        "https://httpbin.org/get",
        "https://httpbin.org/ip",
        "https://httpbin.org/user-agent",
        "https://httpbin.org/headers",
        "https://jsonplaceholder.typicode.com/posts/1",
        "https://jsonplaceholder.typicode.com/users/1",
        "https://api.github.com/zen",
        "https://httpbin.org/uuid"
    };

    const int num_tasks = sizeof(urls) / sizeof(urls[0]);

    printf("=== TurboNet Thread Pool HTTP Client Example ===\n");

    turbo_mutex_init(&g_print_mutex);

    // Create thread pool (0 = auto-detect CPU cores)
    turbo_threadpool_t* pool = turbo_threadpool_create(0);
    if (!pool) {
        fprintf(stderr, "Failed to create thread pool\n");
        return 1;
    }

    printf("Thread pool created with %d workers\n", turbo_threadpool_size(pool));
    printf("Fetching %d URLs...\n\n", num_tasks);

    // Allocate tasks
    http_task_t* tasks = calloc(num_tasks, sizeof(http_task_t));
    for (int i = 0; i < num_tasks; i++) {
        tasks[i].id = i;
        tasks[i].url = urls[i];
    }

    // Submit all tasks
    for (int i = 0; i < num_tasks; i++) {
        turbo_threadpool_submit(pool, fetch_url, &tasks[i]);
    }

    // Wait for completion
    turbo_threadpool_wait(pool);

    // Print summary
    printf("\n=== Results Summary ===\n");
    printf("%-4s %-45s %-8s %-12s %s\n", "ID", "URL", "Status", "Size", "Error");
    printf("%-4s %-45s %-8s %-12s %s\n", "----", "---------------------------------------------", "--------", "------------", "-----");

    for (int i = 0; i < num_tasks; i++) {
        printf("%-4d %-45s %-8d %-12zu %s\n",
               tasks[i].id,
               tasks[i].url,
               tasks[i].status_code,
               tasks[i].body_len,
               tasks[i].error);
    }

    printf("\nCompleted: %d/%d, Success: %d\n", g_completed, num_tasks, g_success);

    // Cleanup
    turbo_threadpool_destroy(pool);
    free(tasks);
    turbo_mutex_destroy(&g_print_mutex);

    return (g_success == num_tasks) ? 0 : 1;
}
