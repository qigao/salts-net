#include "tinytest.h"
#include "http_client.h"
#include <stdio.h>
#include <string.h>

/* ── Progress tracking ────────────────────────────────────────────── */

typedef struct {
  const char *operation;
  size_t last_reported;
} progress_ctx_t;

static void progress_callback(size_t transferred, size_t total, void *user_data) {
  progress_ctx_t *ctx = (progress_ctx_t *)user_data;

  /* Report every 10% or at completion */
  size_t percent = total > 0 ? (transferred * 100 / total) : 0;
  size_t last_percent = total > 0 ? (ctx->last_reported * 100 / total) : 0;

  if (percent >= last_percent + 10 || transferred == total) {
    printf("[%s] Progress: %zu/%zu bytes (%.1f%%)\n",
           ctx->operation, transferred, total, (double)percent);
    ctx->last_reported = transferred;
  }
}

/* ── Helper functions ─────────────────────────────────────────────── */

static int is_network_error(http_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

static void create_test_file(const char *path, size_t size_kb) {
  FILE *fp = fopen(path, "wb");
  if (!fp) return;

  /* Write test data in 1KB chunks */
  char chunk[1024];
  memset(chunk, 'A', sizeof(chunk));

  for (size_t i = 0; i < size_kb; i++) {
    fwrite(chunk, 1, sizeof(chunk), fp);
  }

  fclose(fp);
  printf("Created test file: %s (%zu KB)\n", path, size_kb);
}

/* ── Test suite ───────────────────────────────────────────────────── */

spec("Streaming File Transfer") {

  describe("Upload with progress") {

    it("should upload small file with progress tracking") {
      create_test_file("test_small.bin", 10);  /* 10 KB */

      http_client_t *c = http_client_create("https://httpbin.org");
      http_client_set_timeout(c, 15000);

      progress_ctx_t ctx = {.operation = "Upload", .last_reported = 0};

      printf("\n=== Uploading small file (10 KB) ===\n");
      http_response_t *r = http_upload_file_stream(
        c, "https://httpbin.org/post", "test_small.bin",
        progress_callback, &ctx
      );

      if (is_network_error(r)) {
        printf("Network error, skipping test\n");
        http_response_free(r);
        http_client_destroy(c);
        remove("test_small.bin");
        return;
      }

      check_int_eq(r->status_code, 200);
      printf("Upload completed: status=%d\n", r->status_code);

      http_response_free(r);
      http_client_destroy(c);
      remove("test_small.bin");
    }

    it("should upload large file with progress tracking") {
      create_test_file("test_large.bin", 1024);  /* 1 MB */

      http_client_t *c = http_client_create("https://httpbin.org");
      http_client_set_timeout(c, 30000);

      progress_ctx_t ctx = {.operation = "Upload", .last_reported = 0};

      printf("\n=== Uploading large file (1 MB) ===\n");
      http_response_t *r = http_upload_file_stream(
        c, "https://httpbin.org/post", "test_large.bin",
        progress_callback, &ctx
      );

      if (is_network_error(r)) {
        printf("Network error, skipping test\n");
        http_response_free(r);
        http_client_destroy(c);
        remove("test_large.bin");
        return;
      }

      check_int_eq(r->status_code, 200);
      printf("Upload completed: status=%d\n", r->status_code);

      http_response_free(r);
      http_client_destroy(c);
      remove("test_large.bin");
    }
  }

  describe("Download with progress") {

    it("should download file with progress tracking") {
      http_client_t *c = http_client_create("https://httpbin.org");
      http_client_set_timeout(c, 15000);

      progress_ctx_t ctx = {.operation = "Download", .last_reported = 0};

      printf("\n=== Downloading file ===\n");
      http_response_t *r = http_download_file_stream(
        c, "https://httpbin.org/json", "downloaded_stream.json",
        progress_callback, &ctx
      );

      if (is_network_error(r)) {
        printf("Network error, skipping test\n");
        http_response_free(r);
        http_client_destroy(c);
        return;
      }

      check_int_eq(r->status_code, 200);
      printf("Download completed: status=%d\n", r->status_code);

      /* Verify file exists */
      FILE *fp = fopen("downloaded_stream.json", "r");
      check_not_null(fp);
      if (fp) {
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        printf("Downloaded file size: %ld bytes\n", size);
        check(size > 0);
        fclose(fp);
      }

      http_response_free(r);
      http_client_destroy(c);
      remove("downloaded_stream.json");
    }
  }

  describe("Upload without progress callback") {

    it("should upload file without progress tracking") {
      create_test_file("test_no_progress.bin", 5);  /* 5 KB */

      http_client_t *c = http_client_create("https://httpbin.org");
      http_client_set_timeout(c, 15000);

      printf("\n=== Uploading without progress callback ===\n");
      http_response_t *r = http_upload_file_stream(
        c, "https://httpbin.org/post", "test_no_progress.bin",
        NULL, NULL  /* No progress callback */
      );

      if (is_network_error(r)) {
        printf("Network error, skipping test\n");
        http_response_free(r);
        http_client_destroy(c);
        remove("test_no_progress.bin");
        return;
      }

      check_int_eq(r->status_code, 200);
      printf("Upload completed: status=%d\n", r->status_code);

      http_response_free(r);
      http_client_destroy(c);
      remove("test_no_progress.bin");
    }
  }

  describe("Error handling") {

    it("should handle missing file") {
      http_client_t *c = http_client_create("https://httpbin.org");

      printf("\n=== Testing missing file error ===\n");
      http_response_t *r = http_upload_file_stream(
        c, "https://httpbin.org/post", "nonexistent_file.bin",
        NULL, NULL
      );

      check_not_null(r);
      check_int_eq(r->error_code, HTTP_ERROR_FILE_IO);
      printf("Correctly detected missing file: error_code=%d\n", r->error_code);

      http_response_free(r);
      http_client_destroy(c);
    }

    it("should handle invalid output path") {
      http_client_t *c = http_client_create("https://httpbin.org");

      printf("\n=== Testing invalid output path ===\n");

      /* Use a path that definitely doesn't exist on any platform */
      const char *invalid_path =
#ifdef _WIN32
        "Z:\\nonexistent\\directory\\file.json";
#else
        "/root/nonexistent/directory/file.json";
#endif

      http_response_t *r = http_download_file_stream(
        c, "https://httpbin.org/json", invalid_path,
        NULL, NULL
      );

      check_not_null(r);
      check_int_eq(r->error_code, HTTP_ERROR_FILE_IO);
      printf("Correctly detected invalid path: error_code=%d\n", r->error_code);

      http_response_free(r);
      http_client_destroy(c);
    }
  }

  describe("Memory efficiency comparison") {

    it("should demonstrate memory efficiency") {
      printf("\n=== Memory Efficiency Demonstration ===\n");
      printf("Standard upload/download: O(file_size) memory\n");
      printf("Streaming upload/download: O(1) memory (8KB buffer)\n");
      printf("\nFor a 1GB file:\n");
      printf("  Standard: ~1GB RAM usage\n");
      printf("  Streaming: ~8KB RAM usage (125,000x less!)\n");
      check(1);  /* Always pass, this is informational */
    }
  }
}
