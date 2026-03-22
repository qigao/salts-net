/**
 * @file coro_timeout_example.c
 * @brief Demonstration of coroutine timeouts and sleep.
 */

#include "turbo_coro_socket.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void timeout_task(coro_t* co, void* arg) {
    UNUSED(co);
    coro_context_t* ctx = (coro_context_t*)arg;

    // Test 1: Successful connection with sleep
    printf("[Coro] --- Part 1: Sleep Test ---\n");
    printf("[Coro] Sleeping for 1 second...\n");
    uint64_t start = turbo_hrtime() / 1000000;
    coro_sleep(ctx, 1000);
    uint64_t end = turbo_hrtime() / 1000000;
    printf("[Coro] Woke up! (Slept for %llu ms)\n", (unsigned long long)(end - start));

    // Test 2: Timeout test (Connecting to a blackhole IP)
    printf("\n[Coro] --- Part 2: Timeout Test (Blackhole) ---\n");
    coro_socket_t* client = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);

    // Set a very short timeout (500ms)
    printf("[Coro] Setting timeout to 500ms. Target is 10.255.255.1\n");
    coro_socket_set_timeout(client, 500);

    int r = coro_socket_connect(client, "10.255.255.1", 80);

    if (r == TURBO_ETIMEDOUT) {
        printf("[Coro] SUCCESS: Connection timed out as expected (code: %d)\n", r);
    } else if (r == 0) {
        printf("[Coro] INFO: Connection succeeded immediately. Target might be reachable in your environment.\n");
    } else {
        printf("[Coro] Result (code %d): %s\n", r, turbo_strerror(r));
    }

    coro_socket_destroy(client);

    // Test 3: Fast Fail Test (Refused connection)
    printf("\n[Coro] --- Part 3: Immediate Failure Test (Refused) ---\n");
    client = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
    coro_socket_set_timeout(client, 5000);

    printf("[Coro] Connecting to localhost:999 (should be refused)...\n");
    r = coro_socket_connect(client, "127.0.0.1", 999);

    if (r == TURBO_ECONNREFUSED) {
        printf("[Coro] SUCCESS: Connection refused as expected (code: %d)\n", r);
    } else {
        printf("[Coro] Result: (code: %d): %s\n", r, turbo_strerror(r));
    }

    coro_socket_destroy(client);
    printf("\n[Coro] Example tasks complete.\n");
}

int main() {
    printf("=== Coroutine Timeout Example ===\n");
    printf("[Main] Initializing context\n");
    coro_context_t* ctx = coro_context_create(NULL);

    printf("[Main] Spawning coroutine\n");
    coro_context_spawn(ctx, timeout_task, ctx);

    printf("[Main] Running event loop...\n");
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    printf("[Main] Done.\n");
    return 0;
}
