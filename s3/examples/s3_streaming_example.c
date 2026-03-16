#include "s3/s3_client.h"
#include "s3/s3_credentials.h"
#include <stdio.h>
#include <string.h>

/* ── Progress tracking ────────────────────────────────────────────── */

typedef struct {
    const char *operation;
    const char *filename;
    size_t last_percent;
} s3_progress_ctx_t;

static void s3_progress_callback(size_t transferred, size_t total, void *user_data) {
    s3_progress_ctx_t *ctx = (s3_progress_ctx_t *)user_data;

    if (total == 0) {
        printf("[%s] %s: %zu bytes transferred\n", ctx->operation, ctx->filename, transferred);
        return;
    }

    size_t percent = (transferred * 100) / total;
    size_t last_percent = (ctx->last_percent * 100) / total;

    if (percent >= last_percent + 10 || transferred == total) {
        printf("[%s] %s: %zu/%zu bytes (%.1f%%)\n",
               ctx->operation, ctx->filename, transferred, total, (double)percent);
        ctx->last_percent = transferred;
    }
}

/* ── Helper functions ─────────────────────────────────────────────── */

static void create_test_file(const char *path, size_t size_kb) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        printf("Failed to create test file: %s\n", path);
        return;
    }

    char chunk[1024];
    memset(chunk, 'A', sizeof(chunk));

    for (size_t i = 0; i < size_kb; i++) {
        fwrite(chunk, 1, sizeof(chunk), fp);
    }

    fclose(fp);
    printf("Created test file: %s (%zu KB)\n", path, size_kb);
}

/* ── Main example ─────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 5) {
        printf("Usage: %s <endpoint> <access_key> <secret_key> <bucket>\n", argv[0]);
        printf("Example: %s https://s3.amazonaws.com AKIAIOSFODNN7EXAMPLE wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY my-bucket\n", argv[0]);
        return 1;
    }

    const char *endpoint = argv[1];
    const char *access_key = argv[2];
    const char *secret_key = argv[3];
    const char *bucket = argv[4];

    printf("\n=== S3 Streaming Upload/Download Example ===\n\n");

    /* Create S3 client */
    s3_base_url_t base_url;
    if (s3_parse_url(endpoint, &base_url) != 0) {
        printf("Failed to parse endpoint URL\n");
        return 1;
    }

    s3_credential_provider_t *provider = s3_credentials_static_provider(access_key, secret_key, NULL);
    s3_client_t *client = s3_client_create(NULL, base_url, provider);
    if (!client) {
        printf("Failed to create S3 client\n");
        return 1;
    }

    /* ── Test 1: Upload small file ──────────────────────────────── */

    printf("\n--- Test 1: Upload small file (100 KB) ---\n");
    create_test_file("test_small.bin", 100);

    s3_progress_ctx_t upload_ctx = {
        .operation = "Upload",
        .filename = "test_small.bin",
        .last_percent = 0
    };

    s3_error_t err = s3_put_object_from_file(
        client, bucket, "test_small.bin", "test_small.bin",
        "application/octet-stream",
        s3_progress_callback, &upload_ctx
    );

    if (s3_is_ok(err)) {
        printf("✓ Upload successful\n");
    } else {
        printf("✗ Upload failed: %s\n", s3_error_message(err));
    }

    /* ── Test 2: Upload large file ──────────────────────────────── */

    printf("\n--- Test 2: Upload large file (10 MB) ---\n");
    create_test_file("test_large.bin", 10 * 1024);

    s3_progress_ctx_t upload_large_ctx = {
        .operation = "Upload",
        .filename = "test_large.bin",
        .last_percent = 0
    };

    err = s3_put_object_from_file(
        client, bucket, "test_large.bin", "test_large.bin",
        "application/octet-stream",
        s3_progress_callback, &upload_large_ctx
    );

    if (s3_is_ok(err)) {
        printf("✓ Upload successful\n");
    } else {
        printf("✗ Upload failed: %s\n", s3_error_message(err));
    }

    /* ── Test 3: Download file ──────────────────────────────────── */

    printf("\n--- Test 3: Download file ---\n");

    s3_progress_ctx_t download_ctx = {
        .operation = "Download",
        .filename = "test_small.bin",
        .last_percent = 0
    };

    err = s3_download_object_stream(
        client, bucket, "test_small.bin", "downloaded_small.bin",
        s3_progress_callback, &download_ctx
    );

    if (s3_is_ok(err)) {
        printf("✓ Download successful\n");

        /* Verify file size */
        FILE *fp = fopen("downloaded_small.bin", "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long size = ftell(fp);
            fclose(fp);
            printf("  Downloaded file size: %ld bytes\n", size);
        }
    } else {
        printf("✗ Download failed: %s\n", s3_error_message(err));
    }

    /* ── Test 4: Upload without progress callback ───────────────── */

    printf("\n--- Test 4: Upload without progress callback ---\n");
    create_test_file("test_no_progress.bin", 50);

    err = s3_put_object_from_file(
        client, bucket, "test_no_progress.bin", "test_no_progress.bin",
        "application/octet-stream",
        NULL, NULL  /* No progress callback */
    );

    if (s3_is_ok(err)) {
        printf("✓ Upload successful (no progress tracking)\n");
    } else {
        printf("✗ Upload failed: %s\n", s3_error_message(err));
    }

    /* ── Cleanup ────────────────────────────────────────────────── */

    printf("\n--- Cleanup ---\n");
    s3_remove_object(client, bucket, "test_small.bin");
    s3_remove_object(client, bucket, "test_large.bin");
    s3_remove_object(client, bucket, "test_no_progress.bin");
    printf("✓ Removed test objects from S3\n");

    remove("test_small.bin");
    remove("test_large.bin");
    remove("test_no_progress.bin");
    remove("downloaded_small.bin");
    printf("✓ Removed local test files\n");

    s3_client_destroy(client);
    s3_base_url_free(&base_url);

    printf("\n=== All tests completed ===\n\n");

    printf("Memory Efficiency:\n");
    printf("  Standard upload: O(file_size) - 10MB file = 10MB RAM\n");
    printf("  Streaming upload: O(file_size) - 10MB file = 10MB RAM (current implementation)\n");
    printf("  Note: True streaming (O(1) memory) requires AWS Signature V4 streaming support\n");

    return 0;
}
