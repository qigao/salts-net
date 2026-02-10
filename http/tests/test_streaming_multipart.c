#include "tinytest.h"
#include "http_client.h"
#include "http_client_async.h"
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

typedef struct {
    int done;
    int status_code;
    char last_error[256];
    size_t total_received;
} async_ctx_t;

static void on_complete(http_async_request_t *request,
                        http_async_response_t *response, void *user_data) {
    (void)request;
    async_ctx_t *ctx = (async_ctx_t *)user_data;
    ctx->status_code = response->status_code;
    ctx->total_received = response->body_len;
    ctx->done = 1;
    if (response->error)
        strncpy(ctx->last_error, response->error, sizeof(ctx->last_error) - 1);
}

spec("http streaming multipart") {

    before() { create_test_file(); }
    after() { remove_test_file(); }

    describe("async") {

        it("should upload with content-length") {
            http_async_client_t *client = http_async_client_create();
            check_not_null(client);

            http_async_multipart_form_t *form = http_async_multipart_form_create();
            check_not_null(form);
            check_int_eq(http_async_multipart_form_add_file_path(
                form, "file", TEST_FILE, "application/octet-stream"), 0);

            async_ctx_t ctx = {0};
            http_async_request_t *req = http_async_post_multipart(
                client, "https://httpbin.org/post", form, on_complete, &ctx);
            check_not_null(req);

            int timeout_ms = 15000;
            while (!ctx.done && timeout_ms > 0) {
                turbo_sleep_ms(100);
                timeout_ms -= 100;
            }
            check(ctx.done, "request timed out");
            check_int_eq(ctx.status_code, 200);

            http_async_multipart_form_destroy(form);
            http_async_client_destroy(client);
        }

        it("should upload chunked") {
            http_async_client_t *client = http_async_client_create();
            check_not_null(client);

            http_async_multipart_form_t *form = http_async_multipart_form_create();
            check_not_null(form);
            check_int_eq(http_async_multipart_form_add_file_path(
                form, "file", TEST_FILE, "application/octet-stream"), 0);

            async_ctx_t ctx = {0};
            http_async_request_t *req = http_async_post_multipart_chunked(
                client, "https://httpbin.org/post", form, on_complete, &ctx);
            check_not_null(req);

            int timeout_ms = 15000;
            while (!ctx.done && timeout_ms > 0) {
                turbo_sleep_ms(100);
                timeout_ms -= 100;
            }
            check(ctx.done, "request timed out");
            check_int_eq(ctx.status_code, 200);

            http_async_multipart_form_destroy(form);
            http_async_client_destroy(client);
        }
    }

    describe("sync") {

        it("should upload with content-length") {
            http_client_t *client = http_client_create();
            check_not_null(client);

            http_multipart_form_t *form = http_multipart_form_create();
            check_not_null(form);
            check_int_eq(http_multipart_form_add_file_path(
                form, "file", TEST_FILE, "application/octet-stream"), 0);

            http_response_t *response = http_post_multipart(
                client, "https://httpbin.org/post", form);
            check_not_null(response);
            check_int_eq(response->status_code, 200);
            check_not_null(response->body);

            http_response_free(response);
            http_multipart_form_destroy(form);
            http_client_destroy(client);
        }

        it("should upload chunked") {
            http_client_t *client = http_client_create();
            check_not_null(client);

            http_multipart_form_t *form = http_multipart_form_create();
            check_not_null(form);
            check_int_eq(http_multipart_form_add_file_path(
                form, "file", TEST_FILE, "application/octet-stream"), 0);

            http_response_t *response = http_post_multipart_chunked(
                client, "https://httpbin.org/post", form);
            check_not_null(response);
            check_int_eq(response->status_code, 200);
            check_not_null(response->body);

            http_response_free(response);
            http_multipart_form_destroy(form);
            http_client_destroy(client);
        }
    }
}
