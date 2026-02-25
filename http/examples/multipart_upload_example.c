#include "tinytest.h"
#include "http_coro_client.h"
#include <turbo_coro.h>
#include <string.h>
#include <stdio.h>

/* ── Coro test harness ────────────────────────────────────────────── */

typedef struct { turbo_coro_context_t *ctx; void (*test_fn)(turbo_coro_context_t *ctx); } coro_test_ctx_t;

static void coro_test_entry(turbo_coro_t *co, void *arg) {
  UNUSED(co);
  coro_test_ctx_t *tctx = (coro_test_ctx_t *)arg;
  tctx->test_fn(tctx->ctx);
}

static void run_in_coro(void (*fn)(turbo_coro_context_t *ctx)) {
  turbo_coro_context_t *ctx = turbo_coro_context_create(NULL);
  coro_test_ctx_t tctx = {.ctx = ctx, .test_fn = fn};
  turbo_coro_scheduler_t *sched = turbo_coro_scheduler_create();
  turbo_coro_spawn(sched, coro_test_entry, &tctx);
  turbo_coro_scheduler_run(sched);
  turbo_coro_scheduler_destroy(sched);
  turbo_coro_context_destroy(ctx);
}

/* ── Result struct ────────────────────────────────────────────────── */

static struct {
  int ran, skipped, status_code;
  int body_has_title, body_has_filename;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_multipart_memory(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);

  http_multipart_form_t *form = http_multipart_form_create();
  http_multipart_form_add_field(form, "title", "My Document");

  const char *file_content = "Hello, World!\nThis is a test file.";
  http_multipart_form_add_file(form, "file", "test.txt", "text/plain",
                                     file_content, strlen(file_content));

  http_coro_response_t *response = http_coro_post_multipart(client,
      "https://httpbin.org/post", form);
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_multipart_form_destroy(form);
    http_coro_client_destroy(client);
    return;
  }

  g_result.status_code = response->status_code;
  g_result.body_has_title = (response->body && strstr(response->body, "My Document") != NULL);
  g_result.body_has_filename = (response->body && strstr(response->body, "test.txt") != NULL);

  http_coro_response_free(response);
  http_multipart_form_destroy(form);
  http_coro_client_destroy(client);
}

static void test_multipart_file(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  FILE *fp = fopen("test_upload.txt", "w");
  if (!fp) return;
  fprintf(fp, "This is a test file created for upload demonstration.\n");
  fclose(fp);

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);

  http_multipart_form_t *form = http_multipart_form_create();
  http_multipart_form_add_field(form, "disk_file_desc", "File from disk");
  http_multipart_form_add_file_path(form, "file", "test_upload.txt", "text/plain");

  http_coro_response_t *response = http_coro_post_multipart(client,
      "https://httpbin.org/post", form);
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_multipart_form_destroy(form);
    http_coro_client_destroy(client);
    remove("test_upload.txt");
    return;
  }

  g_result.status_code = response->status_code;
  g_result.body_has_filename = (response->body && strstr(response->body, "test_upload.txt") != NULL);

  http_coro_response_free(response);
  http_multipart_form_destroy(form);
  http_coro_client_destroy(client);
  remove("test_upload.txt");
}

spec("Multipart Form Upload Test") {

  it("should successfully perform a multipart form upload with memory data") {
    run_in_coro(test_multipart_memory);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check_int_eq(g_result.body_has_title, 1);
      check_int_eq(g_result.body_has_filename, 1);
    }
  }

  it("should successfully perform a multipart form upload with a file from path") {
    run_in_coro(test_multipart_file);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.status_code, 200);
      check_int_eq(g_result.body_has_filename, 1);
    }
  }
}
