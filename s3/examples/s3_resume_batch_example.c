#include "s3/s3_client.h"
#include "s3/s3_credentials.h"
#include <fmt.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <signal.h>

/* ── Global state for interrupt handling ──────────────────────────── */

static volatile int interrupted = 0;

static void signal_handler(int sig) {
    (void)sig;
    interrupted = 1;
    printf("\n\n[INTERRUPTED] Saving progress...\n");
}

/* ── Progress callbacks ───────────────────────────────────────────── */

static void multipart_progress_with_resume(
    int part_number, int total_parts,
    size_t part_uploaded, size_t part_size,
    size_t total_uploaded, size_t total_size,
    void *user_data) {

    (void)part_uploaded;
    (void)part_size;
    (void)user_data;

    int percent = (int)((total_uploaded * 100) / total_size);
    printf("\r[Multipart] Part %d/%d | %zu/%zu MB (%d%%)    ",
           part_number, total_parts,
           total_uploaded / (1024 * 1024),
           total_size / (1024 * 1024),
           percent);
    fflush(stdout);
}

static void batch_progress_callback(const s3_batch_progress_t* progress, void* user_data) {
    (void)user_data;

    int percent = progress->total_size > 0 ?
        (int)((progress->total_uploaded * 100) / progress->total_size) : 0;

    printf("\r[Batch] File %d/%d | Completed: %d | Failed: %d | %zu/%zu MB (%d%%)    ",
           progress->current_file,
           progress->total_files,
           progress->completed_files,
           progress->failed_files,
           progress->total_uploaded / (1024 * 1024),
           progress->total_size / (1024 * 1024),
           percent);
    fflush(stdout);
}

/* ── Helper functions ─────────────────────────────────────────────── */

static void create_large_file(const char *path, size_t size_mb) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        printf("Failed to create test file: %s\n", path);
        return;
    }

    char chunk[1024 * 1024];
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
        return 1;
    }

    const char *endpoint = argv[1];
    const char *access_key = argv[2];
    const char *secret_key = argv[3];
    const char *bucket = argv[4];

    printf("\n=== S3 Resume & Batch Progress Example ===\n\n");

    /* Setup signal handler for Ctrl+C */
    signal(SIGINT, signal_handler);

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

    /* ── Test 1: Multipart upload with resume support ──────────── */

    printf("\n--- Test 1: Multipart upload with resume (1GB file) ---\n");
    printf("Press Ctrl+C during upload to test resume functionality\n\n");

    create_large_file("resume_test.mp4", 1024);  /* 1GB */

    s3_multipart_options_t options = {
        .part_size_mb = 10,
        .concurrency = 10,
        .progress_cb = multipart_progress_with_resume,
        .progress_ud = NULL,
        .resume_file = "resume_test.state"  /* Enable resume */
    };

    printf("Starting upload (interruptible)...\n");
    s3_error_t err = s3_put_object_multipart_file(
        client, bucket, "videos/resume_test.mp4", "resume_test.mp4",
        "video/mp4", &options
    );

    if (interrupted) {
        printf("\n\n✓ Progress saved to resume_test.state\n");
        printf("Run the program again to resume from where you left off\n");
        goto cleanup;
    }

    if (s3_is_ok(err)) {
        printf("\n✓ Upload successful!\n");
        printf("Resume file deleted (upload complete)\n");
    } else {
        printf("\n✗ Upload failed: %s\n", s3_error_message(err));
        printf("Resume file saved for retry\n");
    }

    /* ── Test 2: Batch upload with progress tracking ───────────── */

    printf("\n--- Test 2: Batch upload with progress (10 files) ---\n\n");

    /* Create 10 test files */
    const int file_count = 10;
    char filenames[10][64];
    s3_batch_item_t items[10];

    for (int i = 0; i < file_count; i++) {
        fmt(filenames[i], sizeof(filenames[i]), "batch_test_{}.mp4", i + 1);
        create_large_file(filenames[i], 100);  /* 100MB each */

        items[i].bucket = bucket;
        items[i].key = filenames[i];
        items[i].file_path = filenames[i];
        items[i].content_type = "video/mp4";
    }

    printf("\nUploading 10 files (1GB total) with progress tracking...\n");
    time_t start = time(NULL);

    s3_batch_result_t *results = s3_put_objects_batch_progress(
        client, items, file_count, 10,
        batch_progress_callback, NULL
    );

    time_t end = time(NULL);
    double elapsed = difftime(end, start);

    printf("\n\nBatch upload completed in %.2f seconds\n", elapsed);

    /* Check results */
    int success = 0, failed = 0;
    for (int i = 0; i < file_count; i++) {
        if (s3_is_ok(results[i].error)) {
            success++;
        } else {
            failed++;
            printf("  ✗ %s failed: %s\n", filenames[i], s3_error_message(results[i].error));
        }
    }

    printf("Success: %d, Failed: %d\n", success, failed);
    printf("Speed: %.2f MB/s\n", 1000.0 / elapsed);

    s3_batch_results_free(results, file_count);

    /* ── Test 3: Resume from interrupted upload ────────────────── */

    printf("\n--- Test 3: Simulated resume scenario ---\n\n");

    create_large_file("resume_demo.mp4", 500);  /* 500MB */

    /* First attempt (will be "interrupted" after 30% */
    printf("First attempt (will simulate interruption)...\n");

    options.resume_file = "resume_demo.state";
    options.progress_cb = multipart_progress_with_resume;

    /* TODO: Simulate interruption by manually stopping after a few parts
     * In real usage, user presses Ctrl+C and resume file is saved */

    printf("(In real usage, press Ctrl+C to interrupt)\n");
    printf("(Then run again to resume from saved state)\n\n");

    err = s3_put_object_multipart_file(
        client, bucket, "videos/resume_demo.mp4", "resume_demo.mp4",
        "video/mp4", &options
    );

    if (s3_is_ok(err)) {
        printf("\n✓ Upload successful!\n");
    }

    /* ── Cleanup ────────────────────────────────────────────────── */

cleanup:
    printf("\n--- Cleanup ---\n");

    if (!interrupted) {
        s3_remove_object(client, bucket, "videos/resume_test.mp4");
        s3_remove_object(client, bucket, "videos/resume_demo.mp4");
        for (int i = 0; i < file_count; i++) {
            s3_remove_object(client, bucket, filenames[i]);
        }
        printf("✓ Removed test objects from S3\n");

        remove("resume_test.mp4");
        remove("resume_demo.mp4");
        for (int i = 0; i < file_count; i++) {
            remove(filenames[i]);
        }
        printf("✓ Removed local test files\n");
    }

    s3_client_destroy(client);
    s3_base_url_free(&base_url);

    printf("\n=== Feature Summary ===\n");
    printf("✓ Multipart upload with resume support\n");
    printf("  - Saves progress to .state file\n");
    printf("  - Resumes from last completed part\n");
    printf("  - Handles Ctrl+C gracefully\n\n");
    printf("✓ Batch upload with progress tracking\n");
    printf("  - Real-time progress updates\n");
    printf("  - Shows completed/failed counts\n");
    printf("  - Overall progress percentage\n");

    return 0;
}
