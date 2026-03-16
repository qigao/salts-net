#include "http_client.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>

/* ── Global state for interrupt handling ──────────────────────────── */

static volatile int interrupted = 0;

static void signal_handler(int sig) {
    (void)sig;
    interrupted = 1;
    printf("\n\n[INTERRUPTED] Saving progress for resume...\n");
}

/* ── Progress callback ────────────────────────────────────────────── */

typedef struct {
    const char *filename;
    time_t start_time;
    size_t last_reported;
} download_progress_ctx_t;

static void download_progress(size_t downloaded, size_t total, void *user_data) {
    download_progress_ctx_t *ctx = (download_progress_ctx_t *)user_data;

    if (total == 0) {
        printf("\r[%s] Downloaded: %zu KB", ctx->filename, downloaded / 1024);
        fflush(stdout);
        return;
    }

    size_t percent = (downloaded * 100) / total;
    time_t now = time(NULL);
    double elapsed = difftime(now, ctx->start_time);
    double speed_kbps = elapsed > 0 ? (downloaded / 1024.0) / elapsed : 0;

    printf("\r[%s] %zu/%zu KB (%zu%%) | %.2f KB/s    ",
           ctx->filename,
           downloaded / 1024,
           total / 1024,
           percent,
           speed_kbps);
    fflush(stdout);

    ctx->last_reported = downloaded;
}

/* ── Main example ─────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <url> <output_file>\n", argv[0]);
        printf("Example: %s https://example.com/large_file.mp4 video.mp4\n", argv[0]);
        printf("\nFeatures:\n");
        printf("  - Automatic resume on network failure\n");
        printf("  - Press Ctrl+C to interrupt and resume later\n");
        printf("  - Exponential backoff retry (5 attempts)\n");
        printf("  - Progress tracking\n");
        return 1;
    }

    const char *url = argv[1];
    const char *output_file = argv[2];

    printf("\n=== HTTP Resume Download Example ===\n");
    printf("URL: %s\n", url);
    printf("Output: %s\n", output_file);
    printf("\nPress Ctrl+C to interrupt and test resume\n\n");

    /* Setup signal handler */
    signal(SIGINT, signal_handler);

    /* Create HTTP client */
    http_client_t *client = http_client_create(NULL);
    if (!client) {
        printf("Failed to create HTTP client\n");
        return 1;
    }

    http_client_set_timeout(client, 30000);  /* 30 seconds */

    /* ── Test 1: Download with resume support ──────────────────── */

    download_progress_ctx_t ctx = {
        .filename = output_file,
        .start_time = time(NULL),
        .last_reported = 0
    };

    http_resume_options_t options = {
        .resume_file = "download.state",  /* Save progress here */
        .retry_count = 5,                 /* Retry 5 times */
        .retry_delay_ms = 1000            /* Start with 1 second delay */
    };

    printf("Starting download with resume support...\n");

    http_response_t *resp = http_download_file_resume(
        client, url, output_file, &options,
        download_progress, &ctx
    );

    printf("\n\n");

    if (interrupted) {
        printf("✓ Download interrupted\n");
        printf("✓ Progress saved to download.state\n");
        printf("\nRun the same command again to resume:\n");
        printf("  %s %s %s\n", argv[0], url, output_file);
        http_response_free(resp);
        http_client_destroy(client);
        return 0;
    }

    if (resp && resp->error_code == HTTP_ERROR_NONE) {
        printf("✓ Download successful!\n");
        printf("  Status: %d\n", resp->status_code);
        printf("  File: %s\n", output_file);

        /* Check file size */
        FILE *fp = fopen(output_file, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long size = ftell(fp);
            fclose(fp);
            printf("  Size: %ld bytes (%.2f MB)\n", size, size / (1024.0 * 1024.0));
        }

        printf("\n✓ Resume file deleted (download complete)\n");
    } else {
        printf("✗ Download failed\n");
        if (resp) {
            printf("  Error: %s\n", resp->error ? resp->error : "Unknown");
            printf("  Status: %d\n", resp->status_code);
        }
        printf("\n✓ Progress saved to download.state\n");
        printf("  Run again to retry from last position\n");
    }

    /* ── Test 2: Compare with standard download ─────────────────── */

    printf("\n--- Comparison: Standard vs Resume Download ---\n\n");

    printf("Standard download:\n");
    printf("  - No resume support\n");
    printf("  - Network failure = start over\n");
    printf("  - Single attempt\n\n");

    printf("Resume download:\n");
    printf("  - ✓ Automatic resume on failure\n");
    printf("  - ✓ Uses HTTP Range requests\n");
    printf("  - ✓ 5 retry attempts with backoff\n");
    printf("  - ✓ Saves progress to .state file\n");
    printf("  - ✓ Perfect for mobile/unstable networks\n\n");

    printf("Mobile network benefits:\n");
    printf("  - Handles network drops gracefully\n");
    printf("  - Resumes from exact byte position\n");
    printf("  - Exponential backoff prevents server overload\n");
    printf("  - No wasted bandwidth re-downloading\n");

    http_response_free(resp);
    http_client_destroy(client);

    return 0;
}
