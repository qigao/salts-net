/**
 * @file coro_tls_example.c
 * @brief Coroutine-based TLS client connecting to www.google.com:443.
 *
 * Demonstrates the three-step coroutine pattern:
 *   1. Create a CORO_SOCKET_TLS socket (not TCP_V4 — TLS is its own type).
 *   2. Connect — suspends until the TCP+TLS handshake completes.
 *   3. Send / Recv — each call suspends until the kernel is ready.
 *
 * Usage: ./coro_tls_example
 */

#include "CoroNet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void tls_task(coro_t *co, void *arg) {
    UNUSED(co);
    coro_context_t *ctx = (coro_context_t *)arg;

    /* CORO_SOCKET_TLS performs TLS handshake inside coro_socket_connect(). */
    printf("[Coro] Creating TLS socket...\n");
    coro_socket_t *client = coro_socket_create(ctx, CORO_SOCKET_TLS);

    /* Generous timeout: DNS + TCP + TLS handshake may take several seconds. */
    coro_socket_set_timeout(client, 15000);

    printf("[Coro] Connecting to www.google.com:443 (TLS + DNS)...\n");
    int r = coro_socket_connect(client, "www.google.com", 443);
    if (r != 0) {
        printf("[Coro] Connection failed: (code %d) %s\n", r, turbo_strerror(r));
        coro_socket_destroy(client);
        return;
    }
    printf("[Coro] Connected!\n");

    const char *req = "GET / HTTP/1.1\r\nHost: www.google.com\r\nConnection: close\r\n\r\n";
    printf("[Coro] Sending HTTP GET...\n");
    r = coro_socket_send(client, req, strlen(req));
    if (r != 0) {
        printf("[Coro] Send failed: (code %d) %s\n", r, turbo_strerror(r));
        coro_socket_destroy(client);
        return;
    }

    printf("[Coro] Waiting for response...\n");
    char *data = NULL;
    size_t len = 0;
    r = coro_socket_recv(client, &data, &len);
    if (r == 0 && data) {
        /* Print first 500 bytes so the terminal stays readable. */
        size_t to_print = len > 500 ? 500 : len;
        printf("[Coro] Received %zu bytes (showing first %zu):\n", len, to_print);
        printf("--- Response ---\n%.*s\n--- End ---\n", (int)to_print, data);
        coro_socket_free_recv(data);
    } else {
        printf("[Coro] Recv failed: (code %d) %s\n", r, turbo_strerror(r));
    }

    coro_socket_destroy(client);
    printf("[Coro] TLS task complete.\n");
}

int main(void) {
    printf("=== Coroutine TLS Example ===\n");

    coro_context_t *ctx = coro_context_create(NULL);
    coro_context_spawn(ctx, tls_task, ctx);
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    printf("[Main] Done.\n");
    return 0;
}
