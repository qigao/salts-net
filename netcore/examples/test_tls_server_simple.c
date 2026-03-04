/**
 * test_tls_server_simple.c - Simple TLS echo server with ECDSA P-256
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <uv.h>

#include "turbo_tls.h"
#include "turbo_callbacks.h"
#include <asn1/x509_generate.h>

static volatile int stop_flag = 0;

static void signal_handler(int signum) {
    (void)signum;
    printf("\n[Server] Received signal, stopping...\n");
    fflush(stdout);
    stop_flag = 1;
}

static int on_recv(void *conn, const turbo_pool_slice_t *slice, void *user_data) {
    (void)user_data;

    printf("[Server] on_recv called: conn=%p, slice=%p\n", conn, (void*)slice);
    fflush(stdout);

    if (!slice || !slice->data) {
        printf("[Server] NULL slice\n");
        fflush(stdout);
        return 0;
    }

    printf("[Server] Received %zu bytes\n", slice->length);
    fflush(stdout);

    // Simple echo
    turbo_tls_client_t *client = (turbo_tls_client_t *)conn;
    const char *response = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\nHello World!";

    printf("[Server] Sending response...\n");
    fflush(stdout);

    turbo_tls_send(client, response, strlen(response));

    printf("[Server] Response sent\n");
    fflush(stdout);

    return 0;
}

static void on_connect(void *conn, int status, void *user_data) {
    (void)user_data;

    printf("[Server] on_connect called: conn=%p, status=%d\n", conn, status);
    fflush(stdout);

    if (status == 0) {
        printf("[Server] Client connected successfully\n");
        fflush(stdout);

        turbo_tls_client_t *client = (turbo_tls_client_t *)conn;
        int ret = turbo_tls_read_start(client, NULL, on_recv);

        printf("[Server] turbo_tls_read_start returned: %d\n", ret);
        fflush(stdout);
    } else {
        printf("[Server] Client connection failed: %d\n", status);
        fflush(stdout);
    }
}

static void on_close(void *conn, void *user_data) {
    (void)conn;
    (void)user_data;
    printf("[Server] Client disconnected\n");
    fflush(stdout);
}

int main(void) {
    // Enable TLS debug logging
#ifdef _WIN32
    _putenv("TURBO_TLS_DEBUG=1");
#else
    setenv("TURBO_TLS_DEBUG", "1", 1);
#endif

    printf("Simple TLS Server with ECDSA P-256\n");
    printf("===================================\n\n");
    fflush(stdout);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Generate certificate
    printf("1. Generating ECDSA P-256 certificate...\n");
    fflush(stdout);

    char *cert_pem = NULL;
    char *key_pem = NULL;
    size_t cert_len = 0;
    size_t key_len = 0;

    int ret = x509_generate_tls_cert_pem_ecdsa("localhost", 365, &cert_pem, &cert_len, &key_pem, &key_len);
    if (ret != 0) {
        printf("   ERROR: Certificate generation failed\n");
        fflush(stdout);
        return 1;
    }
    printf("   OK (%zu + %zu bytes)\n", cert_len, key_len);
    fflush(stdout);

    // Initialize TLS context
    printf("\n2. Initializing TLS context...\n");
    fflush(stdout);

    turbo_tls_context_t context;
    memset(&context, 0, sizeof(context));

    ret = turbo_tls_context_init(&context, 0);
    if (ret != 0) {
        printf("   ERROR: TLS context init failed: %d\n", ret);
        fflush(stdout);
        goto cleanup;
    }

    ret = turbo_tls_context_set_cert(&context, cert_pem, cert_len);
    if (ret != 0) {
        printf("   ERROR: Set cert failed: %d\n", ret);
        fflush(stdout);
        turbo_tls_context_destroy(&context);
        goto cleanup;
    }

    ret = turbo_tls_context_set_private_key(&context, key_pem, key_len);
    if (ret != 0) {
        printf("   ERROR: Set key failed: %d\n", ret);
        fflush(stdout);
        turbo_tls_context_destroy(&context);
        goto cleanup;
    }

    // Disable client certificate verification for testing
    turbo_tls_context_set_verify_flags(&context, TURBO_TLS_VERIFY_NONE);

    printf("   OK\n");
    fflush(stdout);

    // Create server
    printf("\n3. Starting TLS server on 127.0.0.1:8443...\n");
    fflush(stdout);

    uv_loop_t loop;
    ret = uv_loop_init(&loop);
    if (ret != 0) {
        printf("   ERROR: UV loop init failed: %d\n", ret);
        fflush(stdout);
        turbo_tls_context_destroy(&context);
        goto cleanup;
    }

    turbo_tls_server_t server;
    memset(&server, 0, sizeof(server));

    ret = turbo_tls_server_init(&server, &loop, &context, "127.0.0.1", 8443);
    if (ret != 0) {
        printf("   ERROR: Server init failed: %d\n", ret);
        fflush(stdout);
        uv_loop_close(&loop);
        turbo_tls_context_destroy(&context);
        goto cleanup;
    }

    ret = turbo_tls_server_start(&server, on_recv, on_connect, on_close);
    if (ret != 0) {
        printf("   ERROR: Server start failed: %d\n", ret);
        fflush(stdout);
        uv_loop_close(&loop);
        turbo_tls_context_destroy(&context);
        goto cleanup;
    }

    printf("   OK - Server listening\n");
    printf("\n4. Test with: curl -k https://127.0.0.1:8443/\n");
    printf("   Press Ctrl+C to stop\n\n");
    fflush(stdout);

    // Run server
    while (!stop_flag) {
        uv_run(&loop, UV_RUN_DEFAULT);
    }

    printf("\n5. Shutting down...\n");
    fflush(stdout);

    turbo_tls_server_stop(&server);
    uv_loop_close(&loop);
    turbo_tls_context_destroy(&context);

    printf("   Stopped\n");
    fflush(stdout);

cleanup:
    if (key_pem) {
        memset(key_pem, 0, key_len);
        free(key_pem);
    }
    if (cert_pem) {
        free(cert_pem);
    }

    printf("\nDone.\n");
    fflush(stdout);
    return 0;
}
