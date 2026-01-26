#include "unity.h"
#include "turbo_fs.h"
#include <string.h>
#include <stdio.h>

static char test_file_path[1024];

void setUp(void)
{
    // Construct a test file path
    // For simplicity, just use current dir
    strncpy(test_file_path, "turbo_fs_test_file.txt", sizeof(test_file_path));
    // Ensure it doesn't exist
    turbo_fs_unlink_sync(test_file_path);
}

void tearDown(void)
{
    turbo_fs_unlink_sync(test_file_path);
}

void test_simple_read_write_sync(void)
{
    const char *data = "Hello, TurboNet FS!";
    turbo_fs_buf_t write_buf = turbo_fs_buf_init((char*)data, strlen(data));
    
    int ret = turbo_fs_write_file_sync(test_file_path, &write_buf);
    TEST_ASSERT_EQUAL(0, ret);
    
    turbo_fs_buf_t read_buf = {0};
    ret = turbo_fs_read_file_sync(test_file_path, &read_buf);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(strlen(data), read_buf.len);
    
    // Ensure null termination for string comparison, although read_file_sync does it
    // read_buf.base is guaranteed to be null terminated by turbo_fs_read_file_sync
    TEST_ASSERT_EQUAL_STRING(data, read_buf.base);
    
    turbo_fs_buf_free(&read_buf);
}

void test_streaming_operations(void)
{
    const char *data = "Stream me!";
    size_t len = strlen(data);
    
    // Write using open/write/close
    turbo_file_t fd = turbo_fs_open_sync(test_file_path, TURBO_FS_O_WRONLY | TURBO_FS_O_CREAT | TURBO_FS_O_TRUNC, 0644);
    TEST_ASSERT_NOT_EQUAL(TURBO_INVALID_FILE, fd);
    
    int ret = turbo_fs_write_sync(fd, data, len);
    TEST_ASSERT_EQUAL(len, ret);
    
    turbo_fs_close_sync(fd);
    
    // Read using open/read/close
    fd = turbo_fs_open_sync(test_file_path, TURBO_FS_O_RDONLY, 0);
    TEST_ASSERT_NOT_EQUAL(TURBO_INVALID_FILE, fd);
    
    char buf[64] = {0};
    ret = turbo_fs_read_sync(fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(len, ret);
    // buf is not strictly null terminated by read_sync unless we do it, but we initialized to 0
    TEST_ASSERT_EQUAL_STRING(data, buf);
    
    turbo_fs_close_sync(fd);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_simple_read_write_sync);
    RUN_TEST(test_streaming_operations);
    return UNITY_END();
}
