/**
 * @file bench_coro_net.c
 * @brief Benchmark comparing Coroutines vs OS Threads for high-concurrency I/O.
 */

#include "turbo_coro_client.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <pthread.h>
#endif

#define NUM_CLIENTS 1000
#define SERVER_URL "tcp://127.0.0.1:8080"

typedef struct {
    turbo_coro_context_t* ctx;
    int completed;
} bench_ctx_t;

// --- Coroutine Client Task ---
static void coro_client_task(turbo_coro_t* co, void* arg) {
    UNUSED(co);
    bench_ctx_t* bctx = (bench_ctx_t*)arg;
    turbo_coro_client_t* client = turbo_coro_client_create(bctx->ctx);

    if (turbo_coro_client_connect(client, SERVER_URL) == 0) {
        const char* msg = "bench";
        turbo_coro_client_send(client, msg, 5);

        char* data = NULL;
        size_t len = 0;
        turbo_coro_client_recv(client, &data, &len);
        if (data) free(data);
    }

    turbo_coro_client_destroy(client);
    bctx->completed++;
}

// --- Thread Client Task ---
#ifdef _WIN32
static DWORD WINAPI thread_client_task(LPVOID arg) {
    UNUSED(arg);
    Sleep(100); // Simulate some work
    return 0;
}
#else
static void* thread_client_task(void* arg) {
    UNUSED(arg);
    usleep(100000);
    return NULL;
}
#endif

static size_t get_peak_rss() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info;
    GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info));
    return (size_t)info.PeakWorkingSetSize;
#else
    struct rusage r_usage;
    getrusage(RUSAGE_SELF, &r_usage);
    return (size_t)(r_usage.ru_maxrss * 1024); // KB to bytes
#endif
}

double get_time_ms() {
    return (double)clock() / (CLOCKS_PER_SEC / 1000.0);
}

int main(int argc, char** argv) {
    int mode = (argc > 1) ? atoi(argv[1]) : 0; // 0=Coro, 1=Threads

    printf("Benchmarking with %d clients...\n", NUM_CLIENTS);
    size_t start_rss = get_peak_rss();
    double start_time = get_time_ms();

    if (mode == 0) {
        printf("Mode: COROUTINES (Single Thread)\n");
        turbo_coro_context_t* ctx = turbo_coro_context_create();
        bench_ctx_t bctx = { .ctx = ctx, .completed = 0 };

        // Create 1000 coroutines
        for (int i = 0; i < NUM_CLIENTS; i++) {
            turbo_coro_t* co = turbo_coro_create(coro_client_task, &bctx, NULL);
            turbo_coro_resume(co);
        }

        // Run loop until all complete
        while (bctx.completed < NUM_CLIENTS) {
            turbo_coro_context_run(ctx);
        }

        turbo_coro_context_destroy(ctx);
    } else {
        printf("Mode: OS THREADS\n");
#ifdef _WIN32
        HANDLE threads[NUM_CLIENTS];
        int thread_count = 0;
        for (int i = 0; i < NUM_CLIENTS; i++) {
            threads[i] = CreateThread(NULL, 0, thread_client_task, NULL, 0, NULL);
            if (threads[i] == NULL) {
                printf("Failed to create thread %d, error: %lu\n", i, GetLastError());
                break;
            }
            thread_count++;
        }

        for (int i = 0; i < thread_count; i += 64) {
            int batch_size = (thread_count - i > 64) ? 64 : (thread_count - i);
            WaitForMultipleObjects(batch_size, &threads[i], TRUE, INFINITE);
        }

        for (int i = 0; i < thread_count; i++) CloseHandle(threads[i]);
#else
        pthread_t threads[NUM_CLIENTS];
        for (int i = 0; i < NUM_CLIENTS; i++) {
            pthread_create(&threads[i], NULL, thread_client_task, NULL);
        }
        for (int i = 0; i < NUM_CLIENTS; i++) {
            pthread_join(threads[i], NULL);
        }
#endif
    }

    double end_time = get_time_ms();
    size_t end_rss = get_peak_rss();

    printf("\nResults for %d clients:\n", NUM_CLIENTS);
    printf("Time elapsed: %.2f ms\n", end_time - start_time);
    printf("Peak Memory Increase: %.2f MB\n", (double)(end_rss - start_rss) / (1024.0 * 1024.0));
    printf("Average Memory per Client: %.2f KB\n", (double)(end_rss - start_rss) / (NUM_CLIENTS * 1024.0));

    return 0;
}
