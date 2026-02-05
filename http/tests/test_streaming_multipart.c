/**
 * test_streaming_multipart.c - Tests for streaming multipart uploads
 */

#include "http_client.h"
#include "http_client_async.h"
#include <platform.h>
#include <stdio.h>
#include <string.h>
#include <turbo_fs.h>
#include <unity.h>


#define TEST_FILE "test_streaming.bin"
#define TEST_FILE_SIZE (64 * 1024) // 64KB to test chunking

void setUp(void) {
  /* Create a test file with some data */
  turbo_file_t fd =
      turbo_fs_open(TEST_FILE, TURBO_FS_O_CREAT | TURBO_FS_O_WRONLY | TURBO_FS_O_TRUNC, 0644);
  if (fd != TURBO_INVALID_FILE) {
    char buf[1024];
    memset(buf, 'A', sizeof(buf));
    for (int i = 0; i < TEST_FILE_SIZE / 1024; i++) {
      turbo_fs_write(fd, buf, sizeof(buf));
    }
    turbo_fs_close(fd);
  }
}

void tearDown(void) {
  /* Remove test file */
  remove(TEST_FILE);
}

typedef struct {
  int done;
  int status_code;
  char last_error[256];
  size_t total_received;
} async_test_ctx_t;

static void on_complete(http_async_request_t *request, http_async_response_t *response,
                        void *user_data) {
  async_test_ctx_t *ctx = (async_test_ctx_t *)user_data;
  ctx->status_code = response->status_code;
  ctx->total_received = response->body_len;
  ctx->done = 1;
  if (response->error) {
    strncpy(ctx->last_error, response->error, sizeof(ctx->last_error) - 1);
  }
}

void test_async_streaming_multipart_cl(void) {
  http_async_client_t *client = http_async_client_create();
  TEST_ASSERT_NOT_NULL(client);

  http_async_multipart_form_t *form = http_async_multipart_form_create();
  TEST_ASSERT_NOT_NULL(form);

  int res =
      http_async_multipart_form_add_file_path(form, "file", TEST_FILE, "application/octet-stream");
  TEST_ASSERT_EQUAL_INT(0, res);

  async_test_ctx_t ctx = {0};
  printf("\nStarting Async Streaming Multipart (Content-Length) upload...\n");
  http_async_request_t *req =
      http_async_post_multipart(client, "https://httpbin.org/post", form, on_complete, &ctx);
  TEST_ASSERT_NOT_NULL(req);

  // Wait for completion
  int timeout_ms = 15000;
  while (!ctx.done && timeout_ms > 0) {
    turbo_sleep_ms(100);
    timeout_ms -= 100;
  }

  TEST_ASSERT_TRUE_MESSAGE(ctx.done, "Request timed out");
  if (ctx.status_code != 200) {
    printf("[ERROR] Status %d, Error: %s\n", ctx.status_code, ctx.last_error);
  }
  TEST_ASSERT_EQUAL_INT(200, ctx.status_code);

  http_async_multipart_form_destroy(form);
  http_async_client_destroy(client);
}

void test_async_streaming_multipart_chunked(void) {
  http_async_client_t *client = http_async_client_create();
  TEST_ASSERT_NOT_NULL(client);

  http_async_multipart_form_t *form = http_async_multipart_form_create();
  TEST_ASSERT_NOT_NULL(form);

  int res =
      http_async_multipart_form_add_file_path(form, "file", TEST_FILE, "application/octet-stream");
  TEST_ASSERT_EQUAL_INT(0, res);

  async_test_ctx_t ctx = {0};
  printf("\nStarting Async Streaming Multipart (Chunked) upload...\n");
  http_async_request_t *req = http_async_post_multipart_chunked(client, "https://httpbin.org/post",
                                                                form, on_complete, &ctx);
  TEST_ASSERT_NOT_NULL(req);

  // Wait for completion
  int timeout_ms = 15000;
  while (!ctx.done && timeout_ms > 0) {
    turbo_sleep_ms(100);
    timeout_ms -= 100;
  }

  TEST_ASSERT_TRUE_MESSAGE(ctx.done, "Request timed out");
  if (ctx.status_code != 200) {
    printf("[ERROR] Status %d, Error: %s\n", ctx.status_code, ctx.last_error);
  }
  TEST_ASSERT_EQUAL_INT(200, ctx.status_code);

  http_async_multipart_form_destroy(form);
  http_async_client_destroy(client);
}

void test_sync_streaming_multipart_cl(void) {
  http_client_t *client = http_client_create();
  TEST_ASSERT_NOT_NULL(client);

  http_multipart_form_t *form = http_multipart_form_create();
  TEST_ASSERT_NOT_NULL(form);

  int res = http_multipart_form_add_file_path(form, "file", TEST_FILE, "application/octet-stream");
  TEST_ASSERT_EQUAL_INT(0, res);

  printf("\nStarting Sync Streaming Multipart (Content-Length) upload...\n");
  http_response_t *response = http_post_multipart(client, "https://httpbin.org/post", form);
  TEST_ASSERT_NOT_NULL(response);

  if (response->error) {
    printf("[ERROR] %s\n", response->error);
  }
  TEST_ASSERT_EQUAL_INT(200, response->status_code);
  TEST_ASSERT_NOT_NULL(response->body);

  http_response_free(response);
  http_multipart_form_destroy(form);
  http_client_destroy(client);
}

void test_sync_streaming_multipart_chunked(void) {
  http_client_t *client = http_client_create();
  TEST_ASSERT_NOT_NULL(client);

  http_multipart_form_t *form = http_multipart_form_create();
  TEST_ASSERT_NOT_NULL(form);

  int res = http_multipart_form_add_file_path(form, "file", TEST_FILE, "application/octet-stream");
  TEST_ASSERT_EQUAL_INT(0, res);

  printf("\nStarting Sync Streaming Multipart (Chunked) upload...\n");
  http_response_t *response = http_post_multipart_chunked(client, "https://httpbin.org/post", form);
  TEST_ASSERT_NOT_NULL(response);

  if (response->error) {
    printf("[ERROR] %s\n", response->error);
  }
  TEST_ASSERT_EQUAL_INT(200, response->status_code);
  TEST_ASSERT_NOT_NULL(response->body);

  http_response_free(response);
  http_multipart_form_destroy(form);
  http_client_destroy(client);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_async_streaming_multipart_cl);
  RUN_TEST(test_async_streaming_multipart_chunked);
  RUN_TEST(test_sync_streaming_multipart_cl);
  RUN_TEST(test_sync_streaming_multipart_chunked);

  return UNITY_END();
}
