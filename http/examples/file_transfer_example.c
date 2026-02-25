#include "tinytest.h"
#include "http_coro_client.h"
#include <turbo_coro.h>
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
  int ran, skipped;
  int upload_status, download_status;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test function ───────────────────────────────────────────── */

static void test_file_transfer(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  FILE *fp = fopen("test_upload_file.txt", "w");
  if (!fp) return;
  fprintf(fp, "This is a test file for upload demonstration.");
  fclose(fp);

  http_coro_client_t *client = http_coro_client_create(ctx);
  http_coro_client_set_timeout(client, 10000);

  http_coro_response_t *response = http_coro_upload_file(client, "https://httpbin.org/post", "test_upload_file.txt");
  if (is_network_error(response)) {
    g_result.skipped = 1;
    http_coro_response_free(response);
    http_coro_client_destroy(client);
    remove("test_upload_file.txt");
    return;
  }
  g_result.upload_status = response->status_code;
  http_coro_response_free(response);

  response = http_coro_download_file(client, "https://httpbin.org/json", "downloaded_file.json");
  if (!is_network_error(response))
    g_result.download_status = response->status_code;
  http_coro_response_free(response);

  http_coro_client_destroy(client);
  remove("test_upload_file.txt");
  remove("downloaded_file.json");
}

spec("File Transfer Test") {

  it("should successfully upload and download a file") {
    run_in_coro(test_file_transfer);
    check_int_eq(g_result.ran, 1);
    if (!g_result.skipped) {
      check_int_eq(g_result.upload_status, 200);
      check_int_eq(g_result.download_status, 200);
    }
  }
}
