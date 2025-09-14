/**
 * test_tls_server_ed25519.c - TLS server with Ed25519 self-signed certificate
 *
 * Demonstrates using x509_generate_tls_cert_pem() to generate Ed25519
 * certificates for netcore TLS server.
 */

#include <stdio.h>
#include <string.h>
#include <uv.h>

#include "turbo_tls.h"
#include "turbo_callbacks.h"
#include <asn1/x509_generate.h>

static int on_recv(void *conn, const turbo_arena_slice_t *slice, void *user_data) {
    (void)user_data;

    if (!slice || !slice->data) {
        return 0;
    }

    printf("  [Server] Received %zu bytes: ", slice->length);
    fwrite(slice->data, 1, slice->length > 64 ? 64 : slice->length, stdout);
    if (slice->length > 64) printf("...");
    printf("\n");

    // Echo back to client
    turbo_tls_client_t *client = (turbo_tls_client_t *)conn;
    const char *response = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/plain\r\n"
                           "Content-Length: 27\r\n"
                           "\r\n"
                           "Hello from TLS Server!\r\n";
    turbo_tls_send(client, response, strlen(response));

    // Close connection after sending response
    turbo_tls_client_close(client);

    return 0;
}

static void on_connect(void *conn, int status, void *user_data) {
    (void)user_data;

    if (status == 0) {
        printf("  [Server] Client connected\n");
        turbo_tls_client_t *client = (turbo_tls_client_t *)conn;
        turbo_tls_read_start(client, NULL, on_recv);
    } else {
        printf("  [Server] Client connection failed: %d\n", status);
    }
}

static void on_close(void *conn, void *user_data) {
    (void)conn;
    (void)user_data;
    printf("  [Server] Client disconnected\n");
}

static void on_timeout(uv_timer_t *timer) {
    int *flag = (int *)timer->data;
    *flag = 1;
    uv_stop(timer->loop);
}

int main(void) {
    printf("TLS Server with Ed25519 Certificate\n");
    printf("====================================\n\n");

    // Generate Ed25519 certificate (PEM format)
    printf("1. Generating Ed25519 self-signed certificate...\n");
    fflush(stdout);

    char *cert_pem = NULL;
    char *key_pem = NULL;
    size_t cert_len = 0;
    size_t key_len = 0;

    int ret = x509_generate_tls_cert_pem(
        "localhost",  // Common Name
        365,          // Valid for 1 year
        &cert_pem, &cert_len,
        &key_pem, &key_len
    );

    if (ret != 0) {
        printf("   ERROR: Failed to generate certificate (ret=%d)\n", ret);
        fflush(stdout);
        return 1;
    }

    printf("   Generated certificate (%zu bytes)\n", cert_len);
    printf("   Generated private key (%zu bytes)\n", key_len);
    fflush(stdout);

    if (!cert_pem || !key_pem) {
        printf("   ERROR: Certificate or key is NULL\n");
        fflush(stdout);
        return 1;
    }

    printf("   Certificate preview:\n");

    // Show first few lines of certificate
    const char *line_end = strchr(cert_pem, '\n');
    if (line_end) {
        size_t first_line_len = line_end - cert_pem + 1;
        printf("   %.*s", (int)first_line_len, cert_pem);

        const char *second_line_start = line_end + 1;
        const char *second_line_end = strchr(second_line_start, '\n');
        if (second_line_end) {
            size_t second_line_len = second_line_end - second_line_start + 1;
            printf("   %.*s", (int)second_line_len, second_line_start);
            printf("   ...\n");
        }
    }
    fflush(stdout);

    // Initialize TLS context
    printf("\n2. Initializing TLS context...\n");
    fflush(stdout);

    turbo_tls_context_t context;
    memset(&context, 0, sizeof(context));

    ret = turbo_tls_context_init(&context, 0);
    if (ret != 0) {
        printf("   ERROR: Failed to initialize TLS context (ret=%d)\n", ret);
        fflush(stdout);
        goto cleanup;
    }

    // Set certificate and private key
    printf("   Loading certificate...\n");
    fflush(stdout);

    ret = turbo_tls_context_set_cert(&context, cert_pem, cert_len);
    if (ret != 0) {
        printf("   ERROR: Failed to set certificate (ret=%d)\n", ret);
        fflush(stdout);
        turbo_tls_context_destroy(&context);
        goto cleanup;
    }

    printf("   Loading private key...\n");
    fflush(stdout);

    ret = turbo_tls_context_set_private_key(&context, key_pem, key_len);
    if (ret != 0) {
        printf("   ERROR: Failed to set private key (ret=%d)\n", ret);
        fflush(stdout);
        turbo_tls_context_destroy(&context);
        goto cleanup;
    }

    // Disable client certificate verification for testing
    turbo_tls_context_set_verify_flags(&context, TURBO_TLS_VERIFY_NONE);

    printf("   TLS context ready\n");
    fflush(stdout);

    // Create TLS server
    printf("\n3. Starting TLS server on 127.0.0.1:8444...\n");
    fflush(stdout);

    uv_loop_t loop;
    ret = uv_loop_init(&loop);
    if (ret != 0) {
        printf("   ERROR: Failed to initialize UV loop (ret=%d)\n", ret);
        fflush(stdout);
        turbo_tls_context_destroy(&context);
        goto cleanup;
    }

    turbo_tls_server_t server;
    memset(&server, 0, sizeof(server));

    ret = turbo_tls_server_init(&server, &loop, &context, "127.0.0.1", 8444);
    if (ret != 0) {
        printf("   ERROR: Failed to initialize server (ret=%d)\n", ret);
        fflush(stdout);
        uv_loop_close(&loop);
        turbo_tls_context_destroy(&context);
        goto cleanup;
    }

    ret = turbo_tls_server_start(&server, on_recv, on_connect, on_close);
    if (ret != 0) {
        printf("   ERROR: Failed to start server (ret=%d)\n", ret);
        fflush(stdout);
        uv_loop_close(&loop);
        turbo_tls_context_destroy(&context);
        goto cleanup;
    }

    printf("   Server listening on https://127.0.0.1:8444\n");
    printf("\n4. Server running. Test with:\n");
    printf("   openssl s_client -connect 127.0.0.1:8444 -tls1_3\n");
    printf("   (Ed25519 requires TLS 1.3 - curl may not support it)\n\n");
    fflush(stdout);

    // Run server for 30 seconds
    uv_timer_t timer;
    uv_timer_init(&loop, &timer);

    int timeout_flag = 0;
    timer.data = &timeout_flag;

    uv_timer_start(&timer, on_timeout, 30000, 0);

    while (!timeout_flag) {
        uv_run(&loop, UV_RUN_DEFAULT);
    }

    printf("\n5. Shutting down server...\n");
    fflush(stdout);

    turbo_tls_server_stop(&server);
    uv_loop_close(&loop);
    turbo_tls_context_destroy(&context);

    printf("   Server stopped\n");
    fflush(stdout);

cleanup:
    // Clear and free sensitive data
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
