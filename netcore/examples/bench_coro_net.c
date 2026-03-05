/**
 * @file bench_coro_net.c
 * @brief Benchmark: Coroutines vs OS Threads for high-concurrency I/O.
 *
 * Uses TinyTest's native bench/benchmark infrastructure so results are
 * printed in the same tabular format as the rest of the project's benchmarks.
 *
 * Run modes (controlled by CMake/CTest or manually):
 *   bench_coro_net              -- all benchmarks
 *   bench_coro_net --filter coro -- coroutine bench only
 *   bench_coro_net --filter thread -- thread bench only
 */

#include "turbo_coro_client.h"
#include "turbo_coro.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <psapi.h>
#else
#  include <sys/resource.h>
#  include <pthread.h>
#endif

/* ── Configuration ──────────────────────────────────────────── */

#define NUM_CLIENTS      100   /* clients per benchmark iteration */
#define SERVER_URL       "tcp://127.0.0.1:8080"
#define BENCH_ITERS      3     /* outer repetitions measured by tinytest */

/* ── Memory helpers ─────────────────────────────────────────── */

static size_t get_peak_rss(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info;
    GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info));
    return (size_t)info.PeakWorkingSetSize;
#else
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return (size_t)(r.ru_maxrss * 1024);
#endif
}

/* ── Coroutine client task ──────────────────────────────────── */

typedef struct {
    coro_context_t *ctx;
    int completed;
} bench_ctx_t;

static void coro_client_task(coro_t *co, void *arg) {
    (void)co;
    bench_ctx_t *bctx = (bench_ctx_t *)arg;
    coro_client_t *client = coro_client_create(bctx->ctx);

    if (coro_client_connect(client, SERVER_URL) == 0) {
        const char *msg = "bench";
        coro_client_send(client, msg, 5);

        char  *data = NULL;
        size_t len  = 0;
        coro_client_recv(client, &data, &len);
        if (data) free(data);
    }

    coro_client_destroy(client);
    bctx->completed++;
}

/* ── OS-thread client task ──────────────────────────────────── */

#ifdef _WIN32
static DWORD WINAPI thread_client_task(LPVOID arg) {
    (void)arg;
    Sleep(100); /* simulate network round-trip */
    return 0;
}
#else
static void *thread_client_task(void *arg) {
    (void)arg;
    usleep(100000);
    return NULL;
}
#endif

/* ── Helper: run one batch of coroutine clients ─────────────── */

static void run_coro_batch(void) {
    coro_context_t *ctx = coro_context_create(NULL);
    if (!ctx) return;

    bench_ctx_t bctx = { .ctx = ctx, .completed = 0 };

    for (int i = 0; i < NUM_CLIENTS; i++) {
        coro_context_spawn(ctx, coro_client_task, &bctx);
    }

    /* Drive the loop until all clients have finished */
    while (bctx.completed < NUM_CLIENTS) {
        coro_context_run(ctx, TURBO_RUN_DEFAULT);
    }

    coro_context_destroy(ctx);
}

/* ── Helper: run one batch of OS threads ───────────────────── */

static void run_thread_batch(void) {
#ifdef _WIN32
    HANDLE threads[NUM_CLIENTS];
    int    count = 0;
    for (int i = 0; i < NUM_CLIENTS; i++) {
        threads[i] = CreateThread(NULL, 0, thread_client_task, NULL, 0, NULL);
        if (!threads[i]) {
            fprintf(stderr, "CreateThread failed: %lu\n", GetLastError());
            break;
        }
        count++;
    }
    for (int i = 0; i < count; i += 64) {
        int batch = (count - i > 64) ? 64 : (count - i);
        WaitForMultipleObjects(batch, &threads[i], TRUE, INFINITE);
    }
    for (int i = 0; i < count; i++) CloseHandle(threads[i]);
#else
    pthread_t threads[NUM_CLIENTS];
    for (int i = 0; i < NUM_CLIENTS; i++)
        pthread_create(&threads[i], NULL, thread_client_task, NULL);
    for (int i = 0; i < NUM_CLIENTS; i++)
        pthread_join(threads[i], NULL);
#endif
}

/* ── Benchmarks ─────────────────────────────────────────────── */

spec("Coro vs Threads Network Bench") {

    bench("Coroutine clients (100 concurrent)") {
        static size_t rss_before = 0;
        rss_before = get_peak_rss();

        benchmark("coro_batch", BENCH_ITERS) {
            run_coro_batch();
        }

        size_t rss_after = get_peak_rss();
        printf("    Peak RSS delta: %.2f MB  (~%.2f KB/client)\n",
               (double)(rss_after - rss_before) / (1024.0 * 1024.0),
               (double)(rss_after - rss_before) / (NUM_CLIENTS * 1024.0));
    }

    bench("OS-thread clients (100 concurrent)") {
        static size_t rss_before = 0;
        rss_before = get_peak_rss();

        benchmark("thread_batch", BENCH_ITERS) {
            run_thread_batch();
        }

        size_t rss_after = get_peak_rss();
        printf("    Peak RSS delta: %.2f MB  (~%.2f KB/client)\n",
               (double)(rss_after - rss_before) / (1024.0 * 1024.0),
               (double)(rss_after - rss_before) / (NUM_CLIENTS * 1024.0));
    }
}
