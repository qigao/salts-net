/**
 * @file coro_tls_example.c
 * @brief Demonstration of coroutine-based TLS client with DNS.
 */

#include "turbo_coro_client.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void tls_task(coro_t* co, void* arg) {
    UNUSED(co);
    coro_context_t* ctx = (coro_context_t*)arg;

    printf("[Coro] Creating TLS client...\n");
    coro_client_t* client = coro_client_create(ctx);

    // Set a 15-second timeout for the whole sequence
    coro_client_set_timeout(client, 15000);

    printf("[Coro] Connecting to www.google.com:443 (TLS + DNS)...\n");
    // Now we can use hostnames!
    int r = coro_client_connect(client, "tls://www.google.com:443");

    if (r == 0) {
        printf("[Coro] SUCCESS: Connected to google via TLS!\n");

        const char* req = "GET / HTTP/1.1\r\nHost: www.google.com\r\nConnection: close\r\n\r\n";
        printf("[Coro] Sending HTTP GET request...\n");
        r = coro_client_send(client, req, strlen(req));

        if (r == 0) {
            printf("[Coro] Request sent. Waiting for response...\n");
            char* data = NULL;
            size_t len = 0;
            r = coro_client_recv(client, &data, &len);

            if (r == 0 && data) {
                printf("[Coro] Received %zu bytes response:\n", len);
                // Print first 200 chars
                char buf[501];
                size_t to_print = len > 500 ? 500 : len;
                memcpy(buf, data, to_print);
                buf[to_print] = '\0';
                printf("--- Response (Start) ---\n%s\n--- Response (End) ---\n", buf);
                free(data);
            } else {
                printf("[Coro] Failed to receive response: (code %d) %s\n", r, turbo_strerror(r));
            }
        } else {
            printf("[Coro] Failed to send request: (code %d) %s\n", r, turbo_strerror(r));
        }
    } else {
        printf("[Coro] Connection failed: (code %d) %s\n", r, turbo_strerror(r));
    }

    coro_client_destroy(client);
    printf("[Coro] TLS task complete.\n");
}

int main() {
    printf("[Main] Initializing context\n");
    coro_context_t* ctx = coro_context_create(NULL);

    printf("[Main] Spawning coroutine\n");
    coro_context_spawn(ctx, tls_task, ctx);

    printf("[Main] Starting loop...\n");
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    printf("[Main] Done.\n");
    return 0;
}
