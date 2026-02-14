/**
 * @file test_aio.c
 * @brief Unit tests for turbo_aio async file I/O
 */

#include "turbo_aio.h"
#include "turbo_fs.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>

#define TEST_FILE "test_aio_temp.txt"
#define TEST_DATA "Hello, async I/O!"

static turbo_aio_ctx_t *ctx;
static int callback_count;
static int last_result;

static void on_complete(turbo_aio_op_t *op, int result, void *user_data) {
  callback_count++;
  last_result = result;
  if (user_data) {
    *(int *)user_data = result;
  }
}

spec("AIO Tests") {

  before() {
    ctx = turbo_aio_create(64);
    callback_count = 0;
    last_result = -999;
  }

  after() {
    turbo_aio_destroy(ctx);
    remove(TEST_FILE);
  }

  it("should check if AIO is available") { check(turbo_aio_available()); }

  it("should create and destroy AIO context") {
    turbo_aio_ctx_t *c = turbo_aio_create(32);
    check_not_null(c);
    check_uint_eq(turbo_aio_pending(c), 0);
    turbo_aio_destroy(c);
  }

  it("should perform async write and read") {
    int fd = -1;
    callback_count = 0;

    // Use turbo_aio_open to ensure handle is compatible with async I/O
    int err = turbo_aio_open(ctx, TEST_FILE,
                             TURBO_AIO_O_RDWR | TURBO_AIO_O_CREAT |
                                 TURBO_AIO_O_TRUNC,
                             0644, on_complete, &fd);
    check_int_eq(err, TURBO_AIO_OK);

    if (callback_count == 0) {
      turbo_aio_submit(ctx);
      turbo_aio_poll(ctx, 1, 5000);
    }

    check_int_ne(fd, -1);

    // Reset for write test
    callback_count = 0;

    int write_result = -1;
    err = turbo_aio_write(ctx, fd, TEST_DATA, strlen(TEST_DATA), 0, on_complete,
                          &write_result);
    check_int_eq(err, TURBO_AIO_OK);
    check_uint_eq(turbo_aio_pending(ctx), 1);

    turbo_aio_submit(ctx);
    turbo_aio_poll(ctx, 1, 5000);

    check_int_eq(callback_count, 1);
    check_int_eq(write_result, (int)strlen(TEST_DATA));

    char buf[64] = {0};
    int read_result = -1;
    err = turbo_aio_read(ctx, fd, buf, sizeof(buf) - 1, 0, on_complete,
                         &read_result);
    check_int_eq(err, TURBO_AIO_OK);

    turbo_aio_submit(ctx);
    turbo_aio_poll(ctx, 1, 5000);

    check_int_eq(callback_count, 2);
    check_int_eq(read_result, (int)strlen(TEST_DATA));
    check_str_eq(buf, TEST_DATA);

    // Close using AIO
    callback_count = 0;
    int close_result = 0;
    err = turbo_aio_close(ctx, fd, on_complete, &close_result);
    check_int_eq(err, TURBO_AIO_OK);

    if (callback_count == 0) {
      turbo_aio_submit(ctx);
      turbo_aio_poll(ctx, 1, 5000);
    }
  }

  it("should perform fsync") {
    int fd = -1;
    callback_count = 0;

    int err = turbo_aio_open(ctx, TEST_FILE,
                             TURBO_AIO_O_RDWR | TURBO_AIO_O_CREAT |
                                 TURBO_AIO_O_TRUNC,
                             0644, on_complete, &fd);
    check_int_eq(err, TURBO_AIO_OK);

    if (callback_count == 0) {
      turbo_aio_submit(ctx);
      turbo_aio_poll(ctx, 1, 5000);
    }
    check_int_ne(fd, -1);

    callback_count = 0;

    turbo_aio_write(ctx, fd, TEST_DATA, strlen(TEST_DATA), 0, on_complete, NULL);
    turbo_aio_submit(ctx);
    turbo_aio_poll(ctx, 1, 5000);

    err = turbo_aio_fsync(ctx, fd, on_complete, NULL);
    check_int_eq(err, TURBO_AIO_OK);

    // fsync might be sync on Windows
    if (callback_count == 1) { // 1 from write
      turbo_aio_submit(ctx);
      turbo_aio_poll(ctx, 1, 5000);
    }

    check_int_eq(callback_count, 2);

    err = turbo_aio_close(ctx, fd, on_complete, NULL);
    if (err == TURBO_AIO_OK) {
      turbo_aio_submit(ctx);
      if (callback_count == 2)
        turbo_aio_poll(ctx, 1, 1000);
    }
  }

  it("should poll non-blocking") {
    int completed = turbo_aio_poll(ctx, 0, 0);
    check_int_eq(completed, 0);
  }

  it("should return correct error strings") {
    check_str_eq(turbo_aio_strerror(TURBO_AIO_OK), "Success");
    check_str_eq(turbo_aio_strerror(TURBO_AIO_EINVAL), "Invalid argument");
    check_str_eq(turbo_aio_strerror(TURBO_AIO_ENOMEM), "Out of memory");
    check_str_eq(turbo_aio_strerror(TURBO_AIO_EIO), "I/O error");
  }

  it("should handle invalid arguments") {
    char buf[32];
    check_int_eq(turbo_aio_read(NULL, 0, buf, 32, 0, NULL, NULL),
                 TURBO_AIO_EINVAL);
    check_int_eq(turbo_aio_read(ctx, -1, buf, 32, 0, NULL, NULL),
                 TURBO_AIO_EINVAL);
    check_int_eq(turbo_aio_read(ctx, 0, NULL, 32, 0, NULL, NULL),
                 TURBO_AIO_EINVAL);
    check_int_eq(turbo_aio_write(NULL, 0, buf, 32, 0, NULL, NULL),
                 TURBO_AIO_EINVAL);
    check_int_eq(turbo_aio_open(ctx, NULL, 0, 0, NULL, NULL), TURBO_AIO_EINVAL);
  }
}
