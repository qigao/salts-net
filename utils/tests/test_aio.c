/**
 * @file test_aio.c
 * @brief Unit tests for turbo_aio async file I/O
 */

#include "turbo_aio.h"
#include "turbo_fs.h"
#include <unity.h>
#include <stdio.h>
#include <string.h>

#define TEST_FILE "test_aio_temp.txt"
#define TEST_DATA "Hello, async I/O!"

static turbo_aio_ctx_t *ctx;
static int callback_count;
static int last_result;

void setUp(void) {
    ctx = turbo_aio_create(64);
    callback_count = 0;
    last_result = -999;
}

void tearDown(void) {
    turbo_aio_destroy(ctx);
    remove(TEST_FILE);
}

static void on_complete(turbo_aio_op_t *op, int result, void *user_data) {
    callback_count++;
    last_result = result;
    if (user_data) {
        *(int *)user_data = result;
    }
}

void test_aio_available(void) {
    TEST_ASSERT_TRUE(turbo_aio_available());
}

void test_aio_create_destroy(void) {
    turbo_aio_ctx_t *c = turbo_aio_create(32);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT32(0, turbo_aio_pending(c));
    turbo_aio_destroy(c);
}

void test_aio_write_read(void) {
    int fd = turbo_fs_open_sync(TEST_FILE, 
        TURBO_FS_O_RDWR | TURBO_FS_O_CREAT | TURBO_FS_O_TRUNC, 0644);
    TEST_ASSERT_NOT_EQUAL(TURBO_INVALID_FILE, fd);

    int write_result = -1;
    int err = turbo_aio_write(ctx, fd, TEST_DATA, strlen(TEST_DATA), 0, 
                              on_complete, &write_result);
    TEST_ASSERT_EQUAL(TURBO_AIO_OK, err);
    TEST_ASSERT_EQUAL_UINT32(1, turbo_aio_pending(ctx));

    turbo_aio_submit(ctx);
    turbo_aio_poll(ctx, 1, 5000);

    TEST_ASSERT_EQUAL(1, callback_count);
    TEST_ASSERT_EQUAL((int)strlen(TEST_DATA), write_result);

    char buf[64] = {0};
    int read_result = -1;
    err = turbo_aio_read(ctx, fd, buf, sizeof(buf) - 1, 0, on_complete, &read_result);
    TEST_ASSERT_EQUAL(TURBO_AIO_OK, err);

    turbo_aio_submit(ctx);
    turbo_aio_poll(ctx, 1, 5000);

    TEST_ASSERT_EQUAL(2, callback_count);
    TEST_ASSERT_EQUAL((int)strlen(TEST_DATA), read_result);
    TEST_ASSERT_EQUAL_STRING(TEST_DATA, buf);

    turbo_fs_close_sync(fd);
}

void test_aio_fsync(void) {
    int fd = turbo_fs_open_sync(TEST_FILE,
        TURBO_FS_O_RDWR | TURBO_FS_O_CREAT | TURBO_FS_O_TRUNC, 0644);
    TEST_ASSERT_NOT_EQUAL(TURBO_INVALID_FILE, fd);

    turbo_aio_write(ctx, fd, TEST_DATA, strlen(TEST_DATA), 0, on_complete, NULL);
    turbo_aio_submit(ctx);
    turbo_aio_poll(ctx, 1, 5000);

    int err = turbo_aio_fsync(ctx, fd, on_complete, NULL);
    TEST_ASSERT_EQUAL(TURBO_AIO_OK, err);

    turbo_aio_submit(ctx);
    turbo_aio_poll(ctx, 1, 5000);

    TEST_ASSERT_EQUAL(2, callback_count);

    turbo_fs_close_sync(fd);
}

void test_aio_poll_nonblocking(void) {
    int completed = turbo_aio_poll(ctx, 0, 0);
    TEST_ASSERT_EQUAL(0, completed);
}

void test_aio_strerror(void) {
    TEST_ASSERT_EQUAL_STRING("Success", turbo_aio_strerror(TURBO_AIO_OK));
    TEST_ASSERT_EQUAL_STRING("Invalid argument", turbo_aio_strerror(TURBO_AIO_EINVAL));
    TEST_ASSERT_EQUAL_STRING("Out of memory", turbo_aio_strerror(TURBO_AIO_ENOMEM));
    TEST_ASSERT_EQUAL_STRING("I/O error", turbo_aio_strerror(TURBO_AIO_EIO));
}

void test_aio_invalid_args(void) {
    char buf[32];
    TEST_ASSERT_EQUAL(TURBO_AIO_EINVAL, turbo_aio_read(NULL, 0, buf, 32, 0, NULL, NULL));
    TEST_ASSERT_EQUAL(TURBO_AIO_EINVAL, turbo_aio_read(ctx, -1, buf, 32, 0, NULL, NULL));
    TEST_ASSERT_EQUAL(TURBO_AIO_EINVAL, turbo_aio_read(ctx, 0, NULL, 32, 0, NULL, NULL));
    TEST_ASSERT_EQUAL(TURBO_AIO_EINVAL, turbo_aio_write(NULL, 0, buf, 32, 0, NULL, NULL));
    TEST_ASSERT_EQUAL(TURBO_AIO_EINVAL, turbo_aio_open(ctx, NULL, 0, 0, NULL, NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_aio_available);
    RUN_TEST(test_aio_create_destroy);
    RUN_TEST(test_aio_write_read);
    RUN_TEST(test_aio_fsync);
    RUN_TEST(test_aio_poll_nonblocking);
    RUN_TEST(test_aio_strerror);
    RUN_TEST(test_aio_invalid_args);
    return UNITY_END();
}
