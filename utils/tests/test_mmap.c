/**
 * @file test_mmap.c
 * @brief Tests for turbo_mmap memory-mapped file I/O
 */

#include "turbo_mmap.h"
#include "turbo_fs.h"
#include <unity.h>
#include <stdio.h>
#include <string.h>

static char test_file_path[256];
static const char *test_data = "Hello, memory-mapped world! This is test data for mmap.";

void setUp(void) {
    // Create test file
    turbo_fs_get_tmpdir(test_file_path, sizeof(test_file_path) - 32);
    strcat(test_file_path, "/turbo_mmap_test.txt");

    turbo_fs_buf_t buf = turbo_fs_buf_init((char *)test_data, strlen(test_data));
    int err = turbo_fs_write_file_sync(test_file_path, &buf);
    TEST_ASSERT_EQUAL_INT(0, err);
}

void tearDown(void) {
    turbo_fs_unlink_sync(test_file_path);
}

void test_mmap_init(void) {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    TEST_ASSERT_NULL(mmap.data);
    TEST_ASSERT_EQUAL_size_t(0, mmap.length);
    TEST_ASSERT_FALSE(mmap.is_mapped);
}

void test_mmap_page_size(void) {
    size_t page_size = turbo_mmap_page_size();
    TEST_ASSERT_GREATER_THAN(0, page_size);
    // Page size should be power of 2
    TEST_ASSERT_EQUAL_UINT(0, page_size & (page_size - 1));
}

void test_mmap_open_read(void) {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_READ);
    TEST_ASSERT_EQUAL_INT(TURBO_MMAP_OK, err);
    TEST_ASSERT_TRUE(turbo_mmap_is_open(&mmap));
    TEST_ASSERT_NOT_NULL(turbo_mmap_data(&mmap));
    TEST_ASSERT_EQUAL_size_t(strlen(test_data), turbo_mmap_size(&mmap));

    // Verify content
    TEST_ASSERT_EQUAL_MEMORY(test_data, turbo_mmap_data(&mmap), strlen(test_data));

    turbo_mmap_close(&mmap);
    TEST_ASSERT_FALSE(turbo_mmap_is_open(&mmap));
}

void test_mmap_open_write(void) {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_WRITE);
    TEST_ASSERT_EQUAL_INT(TURBO_MMAP_OK, err);

    // Modify first byte
    char *data = (char *)turbo_mmap_data(&mmap);
    char original = data[0];
    data[0] = 'X';

    // Sync to disk
    err = turbo_mmap_sync(&mmap, false);
    TEST_ASSERT_EQUAL_INT(TURBO_MMAP_OK, err);

    turbo_mmap_close(&mmap);

    // Verify change persisted
    turbo_fs_buf_t buf;
    err = turbo_fs_read_file_sync(test_file_path, &buf);
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_EQUAL_CHAR('X', buf.base[0]);
    turbo_fs_buf_free(&buf);

    // Restore original
    turbo_mmap_init(&mmap);
    err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_WRITE);
    TEST_ASSERT_EQUAL_INT(TURBO_MMAP_OK, err);
    ((char *)turbo_mmap_data(&mmap))[0] = original;
    turbo_mmap_sync(&mmap, false);
    turbo_mmap_close(&mmap);
}

void test_mmap_open_range(void) {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    // Map starting at offset 7 ("memory-mapped world...")
    int err = turbo_mmap_open_range(&mmap, test_file_path, 7, 15, TURBO_MMAP_READ);
    TEST_ASSERT_EQUAL_INT(TURBO_MMAP_OK, err);
    TEST_ASSERT_EQUAL_size_t(15, turbo_mmap_size(&mmap));

    // Verify content at offset
    TEST_ASSERT_EQUAL_MEMORY("memory-mapped w", turbo_mmap_data(&mmap), 15);

    turbo_mmap_close(&mmap);
}

void test_mmap_accessors(void) {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_READ);
    TEST_ASSERT_EQUAL_INT(TURBO_MMAP_OK, err);

    // Test get accessor
    TEST_ASSERT_EQUAL_CHAR('H', turbo_mmap_get(&mmap, 0));
    TEST_ASSERT_EQUAL_CHAR('e', turbo_mmap_get(&mmap, 1));
    TEST_ASSERT_EQUAL_CHAR('l', turbo_mmap_get(&mmap, 2));

    turbo_mmap_close(&mmap);
}

void test_mmap_file_not_found(void) {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err = turbo_mmap_open(&mmap, "/nonexistent/path/file.txt", TURBO_MMAP_READ);
    TEST_ASSERT_EQUAL_INT(TURBO_MMAP_ENOENT, err);
    TEST_ASSERT_FALSE(turbo_mmap_is_open(&mmap));
}

void test_mmap_double_open(void) {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_READ);
    TEST_ASSERT_EQUAL_INT(TURBO_MMAP_OK, err);

    // Try to open again without closing
    err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_READ);
    TEST_ASSERT_EQUAL_INT(TURBO_MMAP_EEXIST, err);

    turbo_mmap_close(&mmap);
}

void test_mmap_strerror(void) {
    TEST_ASSERT_EQUAL_STRING("Success", turbo_mmap_strerror(TURBO_MMAP_OK));
    TEST_ASSERT_EQUAL_STRING("Invalid argument", turbo_mmap_strerror(TURBO_MMAP_EINVAL));
    TEST_ASSERT_EQUAL_STRING("File not found", turbo_mmap_strerror(TURBO_MMAP_ENOENT));
    TEST_ASSERT_EQUAL_STRING("File is empty", turbo_mmap_strerror(TURBO_MMAP_EEMPTY));
}

void test_mmap_close_idempotent(void) {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_READ);
    TEST_ASSERT_EQUAL_INT(TURBO_MMAP_OK, err);

    // Close multiple times should be safe
    turbo_mmap_close(&mmap);
    turbo_mmap_close(&mmap);
    turbo_mmap_close(&mmap);

    TEST_ASSERT_FALSE(turbo_mmap_is_open(&mmap));
}

void test_mmap_advise(void) {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_READ);
    TEST_ASSERT_EQUAL_INT(TURBO_MMAP_OK, err);

    // Advise should succeed (or be no-op on Windows)
    err = turbo_mmap_advise(&mmap, TURBO_MMAP_SEQUENTIAL);
    TEST_ASSERT_EQUAL_INT(TURBO_MMAP_OK, err);

    err = turbo_mmap_advise(&mmap, TURBO_MMAP_RANDOM);
    TEST_ASSERT_EQUAL_INT(TURBO_MMAP_OK, err);

    turbo_mmap_close(&mmap);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_mmap_init);
    RUN_TEST(test_mmap_page_size);
    RUN_TEST(test_mmap_open_read);
    RUN_TEST(test_mmap_open_write);
    RUN_TEST(test_mmap_open_range);
    RUN_TEST(test_mmap_accessors);
    RUN_TEST(test_mmap_file_not_found);
    RUN_TEST(test_mmap_double_open);
    RUN_TEST(test_mmap_strerror);
    RUN_TEST(test_mmap_close_idempotent);
    RUN_TEST(test_mmap_advise);

    return UNITY_END();
}
