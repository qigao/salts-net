#include "tinytest.h"
#include "http_coro_client.h"
#include <turbo_coro.h>
#include <platform.h>
#include <stdio.h>
#include <string.h>
#include <turbo_fs.h>

#define TEST_FILE "test_streaming.bin"
#define TEST_FILE_SIZE (64 * 1024)

static void create_test_file(void) {
    turbo_file_t fd = turbo_fs_open(TEST_FILE,
        TURBO_FS_O_CREAT | TURBO_FS_O_WRONLY | TURBO_FS_O_TRUNC, 0644);
    if (fd != TURBO_INVALID_FILE) {
        char buf[1024];
        memset(buf, 'A', sizeof(buf));
        for (int i = 0; i < TEST_FILE_SIZE / 1024; i++)
            turbo_fs_write(fd, buf, sizeof(buf));
        turbo_fs_close(fd);
    }
}

static void remove_test_file(void) {
    remove(TEST_FILE);
}

/* ── Coro test harness ────────────────────────────────────────────── */

typedef struct { turbo_coro_context_t *ctx; void (*test_fn)(turbo_coro_context_t *ctx); } coro_test_ctx_t;

static void coro_test_entry(turbo_coro_t *co, void *arg) {
  UNUSED(co);
  coro_test_ctx_t *tctx = (coro_test_ctx_t *)arg;
  tctx->test_fn(tctx->ctx);
}

static void run_in_coro(void (*fn)(turbo_coro_context_t *ctx)) {
  turbo_coro_context_t *ctx = turbo_coro_context_create();
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
  int has_body;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test functions ──────────────────────────────────────────── */

static void test_multipart_upload(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 15000);

  http_async_multipart_form_t *form = http_async_multipart_form_create();
  http_async_multipart_form_add_file_path(form, "file", TEST_FILE, "application/octet-stream");

  http_coro_response_t *r = http_coro_post_multipart(c, "https://httpbin.org/post", form);
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_async_multipart_form_destroy(form);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  g_result.has_body = (r->body != NULL);

  http_coro_response_free(r);
  http_async_multipart_form_destroy(form);
  http_coro_client_destroy(c);
}

spec("http streaming multipart") {

    before() { create_test_file(); }
    after() { remove_test_file(); }

    describe("coro upload") {

        it("should upload multipart form with file") {
            run_in_coro(test_multipart_upload);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_int_eq(g_result.status_code, 200);
                check_int_eq(g_result.has_body, 1);
            }
        }
    }
}
