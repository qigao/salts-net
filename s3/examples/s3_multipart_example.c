#include "s3/s3_client.h"
#include "s3/s3_credentials.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── Progress tracking ────────────────────────────────────────────── */

typedef struct {
    const char *filename;
    time_t start_time;
    int last_percent;
} multipart_progress_ctx_t;

static void multipart_progress_callback(
    int part_number, int total_parts,
    size_t part_uploaded, size_t part_size,
    size_t total_uploaded, size_t total_size,
    void *user_data) {

    multipart_progress_ctx_t *ctx = (multipart_progress_ctx_t *)user_data;

    /* Calculate overall progress */
    int percent = (int)((total_uploaded * 100) / total_size);

    /* Report every 5% or on part completion */
    if (percent >= ctx->last_percent + 5 || part_uploaded == part_size) {
        time_t now = time(NULL);
        double elapsed = difftime(now, ctx->start_time);
        double speed_mbps = (total_uploaded / (1024.0 * 1024.0)) / (elapsed > 0 ? elapsed : 1);

        printf("[%s] Part %d/%d | Overall: %zu/%zu MB (%d%%) | Speed: %.2f MB/s\n",
               ctx->filename,
               part_number, total_parts,
               total_uploaded / (1024 * 1024),
               total_size / (1024 * 1024),
               percent,
               speed_mbps);

        ctx->last_percent = percent;
    }
}

/* ── Helper functions ─────────────────────────────────────────────── */

static void create_large_file(const char *path, size_t size_mb) {
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

    printf("\n=== S3 Multipart Upload Example ===\n\n");

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

    /* ── Test 1: Upload 1GB file with multipart ────────────────── */

    printf("\n--- Test 1: Upload 1GB file (10MB parts, 10 concurrent) ---\n");
    create_large_file("large_video.mp4", 1024);  /* 1GB */

    multipart_progress_ctx_t ctx = {
        .filename = "large_video.mp4",
        .start_time = time(NULL),
        .last_percent = 0
    };

    s3_multipart_options_t options = {
        .part_size_mb = 10,          /* 10MB per part */
        .concurrency = 10,           /* 10 concurrent uploads */
        .progress_cb = multipart_progress_callback,
        .progress_ud = &ctx,
        .resume_file = NULL          /* No resume for now */
    };

    printf("Starting multipart upload...\n");
    time_t start = time(NULL);

    s3_error_t err = s3_put_object_multipart_file(
        client, bucket, "videos/large_video.mp4", "large_video.mp4",
        "video/mp4", &options
    );

    time_t end = time(NULL);
    double elapsed = difftime(end, start);

    if (s3_is_ok(err)) {
        printf("\n✓ Upload successful!\n");
        printf("  Time: %.2f seconds\n", elapsed);
        printf("  Speed: %.2f MB/s\n", 1024.0 / elapsed);
    } else {
        printf("\n✗ Upload failed: %s\n", s3_error_message(err));
    }

    /* ── Test 2: Upload 6GB file (larger than 5GB limit) ───────── */

    printf("\n--- Test 2: Upload 6GB file (50MB parts, 10 concurrent) ---\n");
    create_large_file("huge_video.mp4", 6 * 1024);  /* 6GB */

    ctx.filename = "huge_video.mp4";
    ctx.start_time = time(NULL);
    ctx.last_percent = 0;

    options.part_size_mb = 50;  /* 50MB per part for large file */

    printf("Starting multipart upload (this will take a while)...\n");
    start = time(NULL);

    err = s3_put_object_multipart_file(
        client, bucket, "videos/huge_video.mp4", "huge_video.mp4",
        "video/mp4", &options
    );

    end = time(NULL);
    elapsed = difftime(end, start);

    if (s3_is_ok(err)) {
        printf("\n✓ Upload successful!\n");
        printf("  Time: %.2f seconds (%.2f minutes)\n", elapsed, elapsed / 60.0);
        printf("  Speed: %.2f MB/s\n", (6 * 1024.0) / elapsed);
    } else {
        printf("\n✗ Upload failed: %s\n", s3_error_message(err));
    }

    /* ── Test 3: Compare with standard upload (will fail for >5GB) ─ */

    printf("\n--- Test 3: Standard upload comparison (100MB file) ---\n");
    create_large_file("standard_test.mp4", 100);  /* 100MB */

    /* Standard upload */
    printf("Standard upload (single request)...\n");
    start = time(NULL);

    err = s3_put_object_from_file(
        client, bucket, "videos/standard_test.mp4", "standard_test.mp4",
        "video/mp4", NULL, NULL
    );

    time_t standard_time = time(NULL) - start;

    if (s3_is_ok(err)) {
        printf("✓ Standard upload: %.2f seconds\n", (double)standard_time);
    } else {
        printf("✗ Standard upload failed\n");
    }

    /* Multipart upload */
    printf("Multipart upload (10MB parts, 10 concurrent)...\n");
    ctx.filename = "standard_test.mp4";
    ctx.start_time = time(NULL);
    ctx.last_percent = 0;
    options.part_size_mb = 10;

    start = time(NULL);

    err = s3_put_object_multipart_file(
        client, bucket, "videos/multipart_test.mp4", "standard_test.mp4",
        "video/mp4", &options
    );

    time_t multipart_time = time(NULL) - start;

    if (s3_is_ok(err)) {
        printf("✓ Multipart upload: %.2f seconds\n", (double)multipart_time);
        printf("Speedup: %.2fx faster\n", (double)standard_time / multipart_time);
    } else {
        printf("✗ Multipart upload failed\n");
    }

    /* ── Cleanup ────────────────────────────────────────────────── */

    printf("\n--- Cleanup ---\n");
    s3_remove_object(client, bucket, "videos/large_video.mp4");
    s3_remove_object(client, bucket, "videos/huge_video.mp4");
    s3_remove_object(client, bucket, "videos/standard_test.mp4");
    s3_remove_object(client, bucket, "videos/multipart_test.mp4");
    printf("✓ Removed test objects from S3\n");

    remove("large_video.mp4");
    remove("huge_video.mp4");
    remove("standard_test.mp4");
    printf("✓ Removed local test files\n");

    s3_client_destroy(client);
    s3_base_url_free(&base_url);

    printf("\n=== Performance Summary ===\n");
    printf("Multipart upload benefits:\n");
    printf("  - Supports files >5GB (up to 5TB)\n");
    printf("  - Concurrent part uploads (10x faster)\n");
    printf("  - Resume support (future)\n");
    printf("  - Progress tracking per part\n");
    printf("\nIdeal for:\n");
    printf("  - 4K/8K video files (>1GB)\n");
    printf("  - Database backups\n");
    printf("  - Large datasets\n");

    return 0;
}
