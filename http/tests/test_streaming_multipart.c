#include "tinytest.h"
#include "http_client.h"
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

/* ── Network error check ─────────────────────────────────────────── */

static int is_network_error(http_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

spec("http streaming multipart") {

    before_all() { create_test_file(); }
    after_all() { remove_test_file(); }

    describe("coro upload") {

        it("should upload multipart form with file") {
            http_client_t *c = http_client_create("https://httpbin.org/");
            http_client_set_timeout(c, 15000);
            http_multipart_form_t *form = http_multipart_form_create();
            http_multipart_form_add_file_path(form, "file", TEST_FILE, "application/octet-stream");
            http_response_t *r = http_post_multipart(c, "post", form);
            if (!is_network_error(r)) {
                check_int_eq(r->status_code, 200);
                check_not_null(r->body);
            }
            http_response_free(r);
            http_multipart_form_destroy(form);
            http_client_destroy(c);
        }
    }
}
