/**
 * @file test_mmap.c
 * @brief Tests for turbo_mmap memory-mapped file I/O
 */

#include "turbo_fs.h"
#include "turbo_mmap.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>

static char test_file_path[256];
static const char *test_data =
    "Hello, memory-mapped world! This is test data for mmap.";

spec("MMAP Tests") {

  before_all() {
    // Create test file
    turbo_fs_get_tmpdir(test_file_path, sizeof(test_file_path) - 32);
    strcat(test_file_path, "/turbo_mmap_test.txt");

    turbo_fs_buf_t buf = turbo_fs_buf_init((char *)test_data, strlen(test_data));
    int err = turbo_fs_write_file(test_file_path, &buf);
    check_int_eq(err, 0);
  }

  after_all() { turbo_fs_unlink(test_file_path); }

  it("should initialize mmap structure correctly") {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    check_null(mmap.data);
    check_size_eq(mmap.length, 0);
    check(!mmap.is_mapped);
  }

  it("should return a valid page size") {
    size_t page_size = turbo_mmap_page_size();
    check_size_gt(page_size, 0);
    // Page size should be power of 2
    check_uint_eq(page_size & (page_size - 1), 0);
  }

  it("should open file for reading") {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_READ);
    check_int_eq(err, TURBO_MMAP_OK);
    check(turbo_mmap_is_open(&mmap));
    check_not_null(turbo_mmap_data(&mmap));
    check_size_eq(turbo_mmap_size(&mmap), strlen(test_data));

    // Verify content
    check_int_eq(memcmp(test_data, turbo_mmap_data(&mmap), strlen(test_data)),
                 0);

    turbo_mmap_close(&mmap);
    check(!turbo_mmap_is_open(&mmap));
  }

  it("should open file for writing and persist changes") {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_WRITE);
    check_int_eq(err, TURBO_MMAP_OK);

    // Modify first byte
    char *data = (char *)turbo_mmap_data(&mmap);
    char original = data[0];
    data[0] = 'X';

    // Sync to disk
    err = turbo_mmap_sync(&mmap, false);
    check_int_eq(err, TURBO_MMAP_OK);

    turbo_mmap_close(&mmap);

    // Verify change persisted
    turbo_fs_buf_t buf;
    err = turbo_fs_read_file(test_file_path, &buf);
    check_int_eq(err, 0);
    check_int_eq(buf.base[0], 'X');
    turbo_fs_buf_free(&buf);

    // Restore original
    turbo_mmap_init(&mmap);
    err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_WRITE);
    check_int_eq(err, TURBO_MMAP_OK);
    ((char *)turbo_mmap_data(&mmap))[0] = original;
    err = turbo_mmap_sync(&mmap, false);
    check_int_eq(err, TURBO_MMAP_OK);
    turbo_mmap_close(&mmap);
  }

  it("should open a specific range of a file") {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    // Map starting at offset 7 ("memory-mapped world...")
    int err =
        turbo_mmap_open_range(&mmap, test_file_path, 7, 15, TURBO_MMAP_READ);
    check_int_eq(err, TURBO_MMAP_OK);
    check_size_eq(turbo_mmap_size(&mmap), 15);

    // Verify content at offset
    check_int_eq(memcmp("memory-mapped w", turbo_mmap_data(&mmap), 15), 0);

    turbo_mmap_close(&mmap);
  }

  it("should provide byte accessors") {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_READ);
    check_int_eq(err, TURBO_MMAP_OK);

    // Test get accessor
    check_int_eq(turbo_mmap_get(&mmap, 0), 'H');
    check_int_eq(turbo_mmap_get(&mmap, 1), 'e');
    check_int_eq(turbo_mmap_get(&mmap, 2), 'l');

    turbo_mmap_close(&mmap);
  }

  it("should fail when file is not found") {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err =
        turbo_mmap_open(&mmap, "/nonexistent/path/file.txt", TURBO_MMAP_READ);
    check_int_eq(err, TURBO_MMAP_ENOENT);
    check(!turbo_mmap_is_open(&mmap));
  }

  it("should fail on double open") {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_READ);
    check_int_eq(err, TURBO_MMAP_OK);

    // Try to open again without closing
    err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_READ);
    check_int_eq(err, TURBO_MMAP_EEXIST);

    turbo_mmap_close(&mmap);
  }

  it("should return correct error strings") {
    check_str_eq(turbo_mmap_strerror(TURBO_MMAP_OK), "Success");
    check_str_eq(turbo_mmap_strerror(TURBO_MMAP_EINVAL), "Invalid argument");
    check_str_eq(turbo_mmap_strerror(TURBO_MMAP_ENOENT), "File not found");
    check_str_eq(turbo_mmap_strerror(TURBO_MMAP_EEMPTY), "File is empty");
  }

  it("should have idempotent close") {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_READ);
    check_int_eq(err, TURBO_MMAP_OK);

    // Close multiple times should be safe
    turbo_mmap_close(&mmap);
    turbo_mmap_close(&mmap);
    turbo_mmap_close(&mmap);

    check(!turbo_mmap_is_open(&mmap));
  }

  it("should handle advise hints") {
    turbo_mmap_t mmap;
    turbo_mmap_init(&mmap);

    int err = turbo_mmap_open(&mmap, test_file_path, TURBO_MMAP_READ);
    check_int_eq(err, TURBO_MMAP_OK);

    // Advise should succeed (or be no-op on Windows)
    err = turbo_mmap_advise(&mmap, TURBO_MMAP_SEQUENTIAL);
    check_int_eq(err, TURBO_MMAP_OK);

    err = turbo_mmap_advise(&mmap, TURBO_MMAP_RANDOM);
    check_int_eq(err, TURBO_MMAP_OK);

    turbo_mmap_close(&mmap);
  }
}
