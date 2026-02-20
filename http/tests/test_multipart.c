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
  int has_body, body_has_multipart;
} g_result;

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Coro test function ───────────────────────────────────────────── */

static void test_upload_multipart(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);

  http_async_multipart_form_t *form = http_async_multipart_form_create();
  http_async_multipart_form_add_field(form, "name", "Test User");
  const char *content = "Hello from multipart test!";
  http_async_multipart_form_add_file(form, "file", "test.txt", "text/plain",
                                     content, strlen(content));

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
  g_result.body_has_multipart = (r->body && strstr(r->body, "multipart/form-data") != NULL);

  http_coro_response_free(r);
  http_async_multipart_form_destroy(form);
  http_coro_client_destroy(c);
}

spec("http multipart") {

    describe("form creation") {

        it("should create and destroy") {
            http_async_multipart_form_t *form = http_async_multipart_form_create();
            check_not_null(form);
            http_async_multipart_form_destroy(form);
        }

        it("should add text fields") {
            http_async_multipart_form_t *form = http_async_multipart_form_create();
            http_async_multipart_form_add_field(form, "name", "John Doe");
            http_async_multipart_form_add_field(form, "email", "john@example.com");
            check_not_null(form);
            http_async_multipart_form_destroy(form);
        }

        it("should add file data") {
            http_async_multipart_form_t *form = http_async_multipart_form_create();
            const char *data = "This is test file content";
            http_async_multipart_form_add_file(form, "upload", "test.txt", "text/plain",
                                               data, strlen(data));
            check_not_null(form);
            http_async_multipart_form_destroy(form);
        }

        it("should add mixed content") {
            http_async_multipart_form_t *form = http_async_multipart_form_create();
            http_async_multipart_form_add_field(form, "title", "My Upload");
            const char *data = "File content here";
            http_async_multipart_form_add_file(form, "file", "doc.txt", "text/plain",
                                               data, strlen(data));
            check_not_null(form);
            http_async_multipart_form_destroy(form);
        }

        it("should handle binary data") {
            http_async_multipart_form_t *form = http_async_multipart_form_create();
            unsigned char binary[256];
            for (int i = 0; i < 256; i++) binary[i] = (unsigned char)i;
            http_async_multipart_form_add_file(form, "binary", "data.bin",
                                               "application/octet-stream",
                                               binary, sizeof(binary));
            check_not_null(form);
            http_async_multipart_form_destroy(form);
        }
    }

    describe("upload") {

        it("should upload multipart form") {
            run_in_coro(test_upload_multipart);
            check_int_eq(g_result.ran, 1);
            if (!g_result.skipped) {
                check_int_eq(g_result.status_code, 200);
                check_int_eq(g_result.has_body, 1);
                check_int_eq(g_result.body_has_multipart, 1);
            }
        }

        it("should add file from path") {
            FILE *fp = fopen("test_multipart.txt", "w");
            if (!fp) return;
            fprintf(fp, "Test file content for multipart upload\n");
            fclose(fp);

            http_async_multipart_form_t *form = http_async_multipart_form_create();
            int result = http_async_multipart_form_add_file_path(form, "file",
                "test_multipart.txt", "text/plain");
            check_int_eq(result, 0);

            http_async_multipart_form_destroy(form);
            remove("test_multipart.txt");
        }
    }
}
