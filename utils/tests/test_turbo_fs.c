#include "turbo_fs.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>

static char test_file_path[1024];

spec("Turbo FS Tests") {

  before() {
    // Construct a test file path
    // For simplicity, just use current dir
    strncpy(test_file_path, "turbo_fs_test_file.txt", sizeof(test_file_path));
    // Ensure it doesn't exist
    turbo_fs_unlink(test_file_path);
  }

  after() { turbo_fs_unlink(test_file_path); }

  describe("Simple Read/Write Sync") {
    it("should write and read file correctly") {
      const char *data = "Hello, TurboNet FS!";
      turbo_fs_buf_t write_buf = turbo_fs_buf_init((char *)data, strlen(data));

      int ret = turbo_fs_write_file(test_file_path, &write_buf);
      check_int_eq(ret, 0);

      turbo_fs_buf_t read_buf = {0};
      ret = turbo_fs_read_file(test_file_path, &read_buf);
      check_int_eq(ret, 0);
      check_size_eq(read_buf.len, strlen(data));

      // Ensure null termination for string comparison, although read_file does it
      // read_buf.base is guaranteed to be null terminated by turbo_fs_read_file
      check_str_eq(read_buf.base, data);

      turbo_fs_buf_free(&read_buf);
    }
  }

  describe("Streaming Operations") {
    it("should handle open/write/read/close") {
      const char *data = "Stream me!";
      size_t len = strlen(data);

      // Write using open/write/close
      turbo_file_t fd = turbo_fs_open(
          test_file_path, TURBO_FS_O_WRONLY | TURBO_FS_O_CREAT | TURBO_FS_O_TRUNC,
          0644);
      check_int_ne(fd, TURBO_INVALID_FILE);

      int ret = turbo_fs_write(fd, data, len);
      check_int_eq(ret, (int)len);

      turbo_fs_close(fd);

      // Read using open/read/close
      fd = turbo_fs_open(test_file_path, TURBO_FS_O_RDONLY, 0);
      check_int_ne(fd, TURBO_INVALID_FILE);

      char buf[64] = {0};
      ret = turbo_fs_read(fd, buf, sizeof(buf));
      check_int_eq(ret, (int)len);
      // buf is not strictly null terminated by read_sync unless we do it, but we
      // initialized to 0
      check_str_eq(buf, data);

      turbo_fs_close(fd);
    }
  }
}
