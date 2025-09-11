/**
 * TurboNet File System Tests
 * "Simple, direct file I/O without libuv exposure" - Test comprehensive file operations
 */
#include "platform.h"
#include "unity.h"
#include "turbonet_fs.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Test data and paths
static const char* TEST_FILE_PATH = "test_turbo_file.txt";
static const char* TEST_DIR_PATH = "test_turbo_dir";
static const char* TEST_DATA = "Hello, TurboNet File System!\nThis is a test file.\n";
static const char* NONEXISTENT_PATH = "this_file_does_not_exist.txt";

// Internal cleanup functions for file system tests
static void fs_test_cleanup(void) {
    // Clean up any test files - ignore errors since files might not exist
    turbo_fs_unlink_sync(TEST_FILE_PATH);
    turbo_fs_rmdir_sync(TEST_DIR_PATH);
}

// =============================================================================
// Basic File I/O Tests
// =============================================================================

void test_turbo_fs_sync_write_read(void) {
    fs_test_cleanup(); // Clean up before test
    
    turbo_fs_buf_t write_buf, read_buf;
    int err;
    
    // Test data setup
    write_buf.base = (char*)TEST_DATA;
    write_buf.len = strlen(TEST_DATA);
    
    // Write file
    err = turbo_fs_write_file_sync(TEST_FILE_PATH, &write_buf);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Read file back
    memset(&read_buf, 0, sizeof(read_buf));
    err = turbo_fs_read_file_sync(TEST_FILE_PATH, &read_buf);
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_NOT_NULL(read_buf.base);
    TEST_ASSERT_EQUAL_size_t(strlen(TEST_DATA), read_buf.len);
    TEST_ASSERT_EQUAL_STRING(TEST_DATA, read_buf.base);
    
    // Cleanup
    turbo_fs_buf_free(&read_buf);
    fs_test_cleanup(); // Clean up after test
}

void test_turbo_fs_write_empty_file(void) {
    fs_test_cleanup(); // Clean up before test
    
    turbo_fs_buf_t write_buf, read_buf;
    int err;
    
    // Write empty file
    write_buf.base = "";
    write_buf.len = 0;
    
    err = turbo_fs_write_file_sync(TEST_FILE_PATH, &write_buf);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Read it back
    memset(&read_buf, 0, sizeof(read_buf));
    err = turbo_fs_read_file_sync(TEST_FILE_PATH, &read_buf);
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_NOT_NULL(read_buf.base);
    TEST_ASSERT_EQUAL_size_t(0, read_buf.len);
    TEST_ASSERT_EQUAL_STRING("", read_buf.base);
    
    // Cleanup
    turbo_fs_buf_free(&read_buf);
    fs_test_cleanup(); // Clean up after test
}

void test_turbo_fs_overwrite_file(void) {
    fs_test_cleanup(); // Clean up before test
    
    turbo_fs_buf_t write_buf1, write_buf2, read_buf;
    int err;
    
    // Write first content
    write_buf1.base = "First content";
    write_buf1.len = strlen(write_buf1.base);
    
    err = turbo_fs_write_file_sync(TEST_FILE_PATH, &write_buf1);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Overwrite with second content
    write_buf2.base = "Second content is longer";
    write_buf2.len = strlen(write_buf2.base);
    
    err = turbo_fs_write_file_sync(TEST_FILE_PATH, &write_buf2);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Read and verify second content
    memset(&read_buf, 0, sizeof(read_buf));
    err = turbo_fs_read_file_sync(TEST_FILE_PATH, &read_buf);
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_EQUAL_size_t(write_buf2.len, read_buf.len);
    TEST_ASSERT_EQUAL_STRING(write_buf2.base, read_buf.base);
    
    // Cleanup
    turbo_fs_buf_free(&read_buf);
    fs_test_cleanup(); // Clean up after test
}

// =============================================================================
// Directory Operations Tests
// =============================================================================

void test_turbo_fs_sync_directory(void) {
    fs_test_cleanup(); // Clean up before test
    
    int err;
    turbo_fs_stat_t stat;
    
    // Create directory
    err = turbo_fs_mkdir_sync(TEST_DIR_PATH, TURBO_FS_DEFAULT_MODE);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Verify directory exists and is a directory
    err = turbo_fs_stat_sync(TEST_DIR_PATH, &stat);
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_TRUE(stat.is_directory);
    TEST_ASSERT_FALSE(stat.is_file);
    TEST_ASSERT_FALSE(stat.is_symlink);
    
    // Remove directory
    err = turbo_fs_rmdir_sync(TEST_DIR_PATH);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Verify directory no longer exists
    err = turbo_fs_stat_sync(TEST_DIR_PATH, &stat);
    TEST_ASSERT_NOT_EQUAL(0, err);
    
    fs_test_cleanup(); // Clean up after test
}

void test_turbo_fs_mkdir_existing_directory(void) {
    fs_test_cleanup(); // Clean up before test
    
    int err;
    
    // Create directory
    err = turbo_fs_mkdir_sync(TEST_DIR_PATH, TURBO_FS_DEFAULT_MODE);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Try to create same directory again - should fail
    err = turbo_fs_mkdir_sync(TEST_DIR_PATH, TURBO_FS_DEFAULT_MODE);
    TEST_ASSERT_NOT_EQUAL(0, err);
    
    fs_test_cleanup(); // Clean up after test
}

// =============================================================================
// File Statistics Tests  
// =============================================================================

void test_turbo_fs_stat_file(void) {
    turbo_fs_buf_t write_buf;
    turbo_fs_stat_t stat;
    int err;
    
    // Create test file
    write_buf.base = (char*)TEST_DATA;
    write_buf.len = strlen(TEST_DATA);
    err = turbo_fs_write_file_sync(TEST_FILE_PATH, &write_buf);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Get file stats
    err = turbo_fs_stat_sync(TEST_FILE_PATH, &stat);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Verify file properties
    TEST_ASSERT_TRUE(stat.is_file);
    TEST_ASSERT_FALSE(stat.is_directory);
    TEST_ASSERT_FALSE(stat.is_symlink);
    TEST_ASSERT_EQUAL_size_t(strlen(TEST_DATA), (size_t)stat.size);
    
    // Times should be reasonable (not zero, not too far in future)
    TEST_ASSERT_GREATER_THAN_UINT64(0, stat.atime);
    TEST_ASSERT_GREATER_THAN_UINT64(0, stat.mtime);
    TEST_ASSERT_GREATER_THAN_UINT64(0, stat.ctime);
}

void test_turbo_fs_stat_nonexistent(void) {
    turbo_fs_stat_t stat;
    int err;
    
    // Try to stat non-existent file
    err = turbo_fs_stat_sync(NONEXISTENT_PATH, &stat);
    TEST_ASSERT_NOT_EQUAL(0, err);
}

// =============================================================================
// File Deletion Tests
// =============================================================================

void test_turbo_fs_unlink_file(void) {
    turbo_fs_buf_t write_buf;
    turbo_fs_stat_t stat;
    int err;
    
    // Create test file
    write_buf.base = (char*)TEST_DATA;
    write_buf.len = strlen(TEST_DATA);
    err = turbo_fs_write_file_sync(TEST_FILE_PATH, &write_buf);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Verify file exists
    err = turbo_fs_stat_sync(TEST_FILE_PATH, &stat);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Delete file
    err = turbo_fs_unlink_sync(TEST_FILE_PATH);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Verify file no longer exists
    err = turbo_fs_stat_sync(TEST_FILE_PATH, &stat);
    TEST_ASSERT_NOT_EQUAL(0, err);
}

void test_turbo_fs_unlink_nonexistent(void) {
    int err;
    
    // Try to delete non-existent file
    err = turbo_fs_unlink_sync(NONEXISTENT_PATH);
    TEST_ASSERT_NOT_EQUAL(0, err);
}

// =============================================================================
// Buffer Utility Tests
// =============================================================================

void test_turbo_fs_buffer_utilities(void) {
    char test_data[] = "Test buffer data";
    
    // Test buffer initialization
    turbo_fs_buf_t buf = turbo_fs_buf_init(test_data, strlen(test_data));
    TEST_ASSERT_EQUAL_PTR(test_data, buf.base);
    TEST_ASSERT_EQUAL_size_t(strlen(test_data), buf.len);
    
    // Test buffer with malloc'd data
    char* malloc_data = malloc(100);
    strcpy(malloc_data, "Malloc'd data");
    
    turbo_fs_buf_t malloc_buf = turbo_fs_buf_init(malloc_data, strlen(malloc_data));
    TEST_ASSERT_EQUAL_PTR(malloc_data, malloc_buf.base);
    TEST_ASSERT_EQUAL_size_t(strlen(malloc_data), malloc_buf.len);
    
    // Test buffer free
    turbo_fs_buf_free(&malloc_buf);
    TEST_ASSERT_NULL(malloc_buf.base);
    TEST_ASSERT_EQUAL_size_t(0, malloc_buf.len);
    
    // Test with NULL buffer (should not crash)
    turbo_fs_buf_free(NULL);
}

// =============================================================================
// Path Utility Tests
// =============================================================================

void test_turbo_fs_path_utilities(void) {
    char result[512];
    int err;
    
    // Test absolute path detection
#ifdef _WIN32
    TEST_ASSERT_TRUE(turbo_fs_path_is_absolute("C:\\test\\path"));
    TEST_ASSERT_TRUE(turbo_fs_path_is_absolute("\\\\server\\share"));
    TEST_ASSERT_FALSE(turbo_fs_path_is_absolute("relative\\path"));
#else
    TEST_ASSERT_TRUE(turbo_fs_path_is_absolute("/test/path"));
    TEST_ASSERT_FALSE(turbo_fs_path_is_absolute("relative/path"));
#endif
    TEST_ASSERT_FALSE(turbo_fs_path_is_absolute(""));
    TEST_ASSERT_FALSE(turbo_fs_path_is_absolute(NULL));
    
    // Test path joining
#ifdef _WIN32
    err = turbo_fs_path_join(result, sizeof(result), "C:\\base", "file.txt");
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_EQUAL_STRING("C:\\base\\file.txt", result);
    
    err = turbo_fs_path_join(result, sizeof(result), "C:\\base\\", "file.txt");
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_EQUAL_STRING("C:\\base\\file.txt", result);
#else
    err = turbo_fs_path_join(result, sizeof(result), "/base", "file.txt");
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_EQUAL_STRING("/base/file.txt", result);
    
    err = turbo_fs_path_join(result, sizeof(result), "/base/", "file.txt");
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_EQUAL_STRING("/base/file.txt", result);
#endif
    
    // Test dirname
#ifdef _WIN32
    err = turbo_fs_path_dirname("C:\\test\\file.txt", result, sizeof(result));
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_EQUAL_STRING("C:\\test", result);
#else
    err = turbo_fs_path_dirname("/test/file.txt", result, sizeof(result));
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_EQUAL_STRING("/test", result);
#endif
    
    err = turbo_fs_path_dirname("file.txt", result, sizeof(result));
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_EQUAL_STRING(".", result);
    
    // Test basename
#ifdef _WIN32
    err = turbo_fs_path_basename("C:\\test\\file.txt", result, sizeof(result));
#else
    err = turbo_fs_path_basename("/test/file.txt", result, sizeof(result));
#endif
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_EQUAL_STRING("file.txt", result);
    
    err = turbo_fs_path_basename("file.txt", result, sizeof(result));
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_EQUAL_STRING("file.txt", result);
}

// =============================================================================
// Temporary Directory Tests
// =============================================================================

void test_turbo_fs_get_tmpdir(void) {
    char tmpdir[512];
    int err;
    
    // Get temporary directory
    err = turbo_fs_get_tmpdir(tmpdir, sizeof(tmpdir));
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_GREATER_THAN_size_t(0, strlen(tmpdir));
    
    // Should be an absolute path
    TEST_ASSERT_TRUE(turbo_fs_path_is_absolute(tmpdir));
    
    // Should exist and be a directory
    turbo_fs_stat_t stat;
    err = turbo_fs_stat_sync(tmpdir, &stat);
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_TRUE(stat.is_directory);
}

// =============================================================================
// Error Handling Tests
// =============================================================================

void test_turbo_fs_error_conditions(void) {
    turbo_fs_buf_t buf;
    turbo_fs_stat_t stat;
    char result[10];
    int err;
    
    // Test NULL parameters
    err = turbo_fs_read_file_sync(NULL, &buf);
    TEST_ASSERT_NOT_EQUAL(0, err);
    
    err = turbo_fs_write_file_sync(NULL, &buf);
    TEST_ASSERT_NOT_EQUAL(0, err);
    
    err = turbo_fs_stat_sync(NULL, &stat);
    TEST_ASSERT_NOT_EQUAL(0, err);
    
    err = turbo_fs_mkdir_sync(NULL, 0755);
    TEST_ASSERT_NOT_EQUAL(0, err);
    
    err = turbo_fs_rmdir_sync(NULL);
    TEST_ASSERT_NOT_EQUAL(0, err);
    
    err = turbo_fs_unlink_sync(NULL);
    TEST_ASSERT_NOT_EQUAL(0, err);
    
    err = turbo_fs_get_tmpdir(NULL, 100);
    TEST_ASSERT_NOT_EQUAL(0, err);
    
    // Test buffer too small for path operations
    err = turbo_fs_path_join(result, sizeof(result), "very_long_base_path", "very_long_filename.txt");
    TEST_ASSERT_NOT_EQUAL(0, err);
    
    err = turbo_fs_path_dirname("/very/long/path/that/exceeds/buffer", result, sizeof(result));
    TEST_ASSERT_NOT_EQUAL(0, err);
    
    err = turbo_fs_path_basename("/very/long/filename_that_exceeds_buffer.txt", result, sizeof(result));
    TEST_ASSERT_NOT_EQUAL(0, err);
}

// =============================================================================
// Large File Tests
// =============================================================================

void test_turbo_fs_large_file_handling(void) {
    turbo_fs_buf_t write_buf, read_buf;
    int err;
    const size_t large_size = 1024 * 1024; // 1MB
    
    // Create large test data
    char* large_data = malloc(large_size);
    TEST_ASSERT_NOT_NULL(large_data);
    
    // Fill with pattern
    for (size_t i = 0; i < large_size; i++) {
        large_data[i] = (char)('A' + (i % 26));
    }
    
    write_buf.base = large_data;
    write_buf.len = large_size;
    
    // Write large file
    err = turbo_fs_write_file_sync(TEST_FILE_PATH, &write_buf);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Read it back
    memset(&read_buf, 0, sizeof(read_buf));
    err = turbo_fs_read_file_sync(TEST_FILE_PATH, &read_buf);
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_NOT_NULL(read_buf.base);
    TEST_ASSERT_EQUAL_size_t(large_size, read_buf.len);
    
    // Verify content
    TEST_ASSERT_EQUAL_MEMORY(large_data, read_buf.base, large_size);
    
    // Cleanup
    free(large_data);
    turbo_fs_buf_free(&read_buf);
}

// =============================================================================
// Binary Data Tests
// =============================================================================

void test_turbo_fs_binary_data(void) {
    turbo_fs_buf_t write_buf, read_buf;
    int err;
    
    // Create binary test data with all byte values
    char binary_data[256];
    for (int i = 0; i < 256; i++) {
        binary_data[i] = (char)i;
    }
    
    write_buf.base = binary_data;
    write_buf.len = sizeof(binary_data);
    
    // Write binary file
    err = turbo_fs_write_file_sync(TEST_FILE_PATH, &write_buf);
    TEST_ASSERT_EQUAL_INT(0, err);
    
    // Read it back
    memset(&read_buf, 0, sizeof(read_buf));
    err = turbo_fs_read_file_sync(TEST_FILE_PATH, &read_buf);
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_NOT_NULL(read_buf.base);
    TEST_ASSERT_EQUAL_size_t(sizeof(binary_data), read_buf.len);
    
    // Verify binary content
    TEST_ASSERT_EQUAL_MEMORY(binary_data, read_buf.base, sizeof(binary_data));
    
    // Cleanup
    turbo_fs_buf_free(&read_buf);
}