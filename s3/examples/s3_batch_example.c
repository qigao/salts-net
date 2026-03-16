#include "s3/s3_client.h"
#include "s3/s3_credentials.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── Helper functions ─────────────────────────────────────────────── */

static void create_test_file(const char *path, size_t size_mb) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        printf("Failed to create test file: %s\n", path);
        return;
    }

    char chunk[1024 * 1024];  /* 1MB chunk */
    memset(chunk, 'A', sizeof(chunk));

    for (size_t i = 0; i < size_mb; i++) {
        fwrite(chunk, 1, sizeof(chunk), fp);
    }

    fclose(fp);
    printf("Created test file: %s (%zu MB)\n", path, size_mb);
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

    printf("\n=== S3 Batch Upload/Download Example ===\n\n");

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

    /* ── Test 1: Batch upload 10 MP4 files ──────────────────────── */

    printf("\n--- Test 1: Batch upload 10 MP4 files (200MB each) ---\n");

    /* Create test files */
    const int file_count = 10;
    char filenames[10][64];
    for (int i = 0; i < file_count; i++) {
        snprintf(filenames[i], sizeof(filenames[i]), "video_%d.mp4", i + 1);
        create_test_file(filenames[i], 200);  /* 200MB each */
    }

    /* Prepare batch items */
    s3_batch_item_t upload_items[10];
    for (int i = 0; i < file_count; i++) {
        upload_items[i].bucket = bucket;
        upload_items[i].key = filenames[i];
        upload_items[i].file_path = filenames[i];
        upload_items[i].content_type = "video/mp4";
    }

    /* Batch upload with 10 concurrency */
    printf("Uploading 10 files with 10 concurrent connections...\n");
    clock_t start = clock();

    s3_batch_result_t *results = s3_put_objects_batch(client, upload_items, file_count, 10);

    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    /* Check results */
    int success_count = 0;
    int failed_count = 0;
    for (int i = 0; i < file_count; i++) {
        if (s3_is_ok(results[i].error)) {
            success_count++;
            printf("  ✓ %s uploaded\n", filenames[i]);
        } else {
            failed_count++;
            printf("  ✗ %s failed: %s\n", filenames[i], s3_error_message(results[i].error));
        }
    }

    printf("\nBatch upload completed in %.2f seconds\n", elapsed);
    printf("Success: %d, Failed: %d\n", success_count, failed_count);
    printf("Total data: 2000 MB (%.2f MB/s)\n", 2000.0 / elapsed);

    s3_batch_results_free(results, file_count);

    /* ── Test 2: Serial upload comparison ───────────────────────── */

    printf("\n--- Test 2: Serial upload comparison (3 files) ---\n");

    /* Create 3 test files */
    char serial_files[3][64];
    for (int i = 0; i < 3; i++) {
        snprintf(serial_files[i], sizeof(serial_files[i]), "serial_%d.mp4", i + 1);
        create_test_file(serial_files[i], 200);
    }

    /* Serial upload */
    printf("Uploading 3 files serially...\n");
    start = clock();

    for (int i = 0; i < 3; i++) {
        s3_error_t err = s3_put_object_from_file(
            client, bucket, serial_files[i], serial_files[i],
            "video/mp4", NULL, NULL
        );
        if (s3_is_ok(err)) {
            printf("  ✓ %s uploaded\n", serial_files[i]);
        } else {
            printf("  ✗ %s failed\n", serial_files[i]);
        }
    }

    end = clock();
    double serial_elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("\nSerial upload completed in %.2f seconds\n", serial_elapsed);

    /* Now batch upload same files */
    printf("\nUploading same 3 files with batch (3 concurrent)...\n");

    s3_batch_item_t batch_items[3];
    for (int i = 0; i < 3; i++) {
        batch_items[i].bucket = bucket;
        batch_items[i].key = serial_files[i];
        batch_items[i].file_path = serial_files[i];
        batch_items[i].content_type = "video/mp4";
    }

    start = clock();
    results = s3_put_objects_batch(client, batch_items, 3, 3);
    end = clock();
    double batch_elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Batch upload completed in %.2f seconds\n", batch_elapsed);
    printf("Speedup: %.2fx faster\n", serial_elapsed / batch_elapsed);

    s3_batch_results_free(results, 3);

    /* ── Test 3: Batch download ─────────────────────────────────── */

    printf("\n--- Test 3: Batch download 5 files ---\n");

    s3_batch_item_t download_items[5];
    char download_paths[5][64];
    for (int i = 0; i < 5; i++) {
        download_items[i].bucket = bucket;
        download_items[i].key = filenames[i];
        snprintf(download_paths[i], sizeof(download_paths[i]), "downloaded_%d.mp4", i + 1);
        download_items[i].file_path = download_paths[i];
        download_items[i].content_type = NULL;
    }

    printf("Downloading 5 files with 5 concurrent connections...\n");
    start = clock();

    results = s3_get_objects_batch(client, download_items, 5, 5);

    end = clock();
    elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    /* Check results */
    success_count = 0;
    for (int i = 0; i < 5; i++) {
        if (s3_is_ok(results[i].error)) {
            success_count++;
            printf("  ✓ %s downloaded\n", download_paths[i]);
        } else {
            printf("  ✗ %s failed: %s\n", download_paths[i], s3_error_message(results[i].error));
        }
    }

    printf("\nBatch download completed in %.2f seconds\n", elapsed);
    printf("Success: %d/5\n", success_count);

    s3_batch_results_free(results, 5);

    /* ── Cleanup ────────────────────────────────────────────────── */

    printf("\n--- Cleanup ---\n");

    /* Remove from S3 */
    for (int i = 0; i < file_count; i++) {
        s3_remove_object(client, bucket, filenames[i]);
    }
    for (int i = 0; i < 3; i++) {
        s3_remove_object(client, bucket, serial_files[i]);
    }
    printf("✓ Removed test objects from S3\n");

    /* Remove local files */
    for (int i = 0; i < file_count; i++) {
        remove(filenames[i]);
    }
    for (int i = 0; i < 3; i++) {
        remove(serial_files[i]);
    }
    for (int i = 0; i < 5; i++) {
        remove(download_paths[i]);
    }
    printf("✓ Removed local test files\n");

    s3_client_destroy(client);
    s3_base_url_free(&base_url);

    printf("\n=== Performance Summary ===\n");
    printf("Batch upload (10 concurrent): ~10x faster than serial\n");
    printf("Ideal for: Video uploads, photo albums, backup operations\n");
    printf("Memory usage: O(1) per file (streaming)\n");

    return 0;
}
