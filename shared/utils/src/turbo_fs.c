/**
 * TurboNet File System Implementation - Synchronous Operations Only
 * "Simple, direct, no bullshit" - Clean file I/O without libuv exposure
 */
#include "platform.h"

#include "tlog.h"
#include "turbo_fs.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>

// Windows compatibility for POSIX file type macros
#ifdef _WIN32
  #include <io.h>
  #include <sys/stat.h>

  #ifndef S_ISREG
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
  #endif
  #ifndef S_ISDIR
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
  #endif
  #ifndef S_ISLNK
    #define S_ISLNK(m) 0 // Windows doesn't have symlinks in the same way
  #endif
#else
  #include <sys/stat.h>
  #include <unistd.h>
#endif

// =============================================================================
// Synchronous File Operations - simple blocking I/O
// =============================================================================

int turbo_fs_read_file(const char *path, turbo_fs_buf_t *buf) {
  if (!path || !buf) {
    return UV_EINVAL;
  }

  uv_fs_t open_req, stat_req, read_req, close_req;
  int err;

  // Get file size first
  err = uv_fs_stat(NULL, &stat_req, path, NULL);
  if (err < 0) {
    TLOG_ERROR("Failed to stat file {}: {}", path, uv_strerror(err));
    return err;
  }

  size_t file_size = (size_t)stat_req.statbuf.st_size;
  uv_fs_req_cleanup(&stat_req);

  // Open file
  err = uv_fs_open(NULL, &open_req, path, UV_FS_O_RDONLY, 0, NULL);
  if (err < 0) {
    TLOG_ERROR("Failed to open file {}: {}", path, uv_strerror(err));
    return err;
  }

  uv_file fd = (uv_file)open_req.result;
  uv_fs_req_cleanup(&open_req);

  // Allocate buffer
  buf->base = malloc(file_size + 1); // +1 for null terminator
  if (!buf->base) {
    uv_fs_close(NULL, &close_req, fd, NULL);
    uv_fs_req_cleanup(&close_req);
    return UV_ENOMEM;
  }

  buf->len = file_size;

  // Read file
  uv_buf_t uv_buf = uv_buf_init(buf->base, (unsigned int)file_size);
  err = uv_fs_read(NULL, &read_req, fd, &uv_buf, 1, 0, NULL);

  if (err < 0) {
    TLOG_ERROR("Failed to read file {}: {}", path, uv_strerror(err));
    free(buf->base);
    buf->base = NULL;
    buf->len = 0;
  } else {
    buf->len = (size_t)read_req.result;
    buf->base[buf->len] = '\0'; // Null terminate
    TLOG_DEBUG("Read file {}: {} bytes", path, buf->len);
  }

  uv_fs_req_cleanup(&read_req);

  // Close file
  uv_fs_close(NULL, &close_req, fd, NULL);
  uv_fs_req_cleanup(&close_req);

  return err < 0 ? err : 0;
}

int turbo_fs_write_file(const char *path, const turbo_fs_buf_t *buf) {
  if (!path || !buf || !buf->base) {
    return UV_EINVAL;
  }

  uv_fs_t open_req, write_req, close_req;
  int err;

  // Open file for writing (create if not exists, truncate if exists)
  err = uv_fs_open(NULL, &open_req, path, UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC,
                   TURBO_FS_DEFAULT_MODE, NULL);
  if (err < 0) {
    TLOG_ERROR("Failed to open file {}: {}", path, uv_strerror(err));
    return err;
  }

  uv_file fd = (uv_file)open_req.result;
  uv_fs_req_cleanup(&open_req);

  // Write file
  uv_buf_t uv_buf = uv_buf_init(buf->base, (unsigned int)buf->len);
  err = uv_fs_write(NULL, &write_req, fd, &uv_buf, 1, 0, NULL);

  if (err < 0) {
    TLOG_ERROR("Failed to write file {}: {}", path, uv_strerror(err));
  } else {
    TLOG_DEBUG("Wrote file {}: {} bytes", path, buf->len);
  }

  uv_fs_req_cleanup(&write_req);

  // Close file
  uv_fs_close(NULL, &close_req, fd, NULL);
  uv_fs_req_cleanup(&close_req);

  return err < 0 ? err : 0;
}

int turbo_fs_stat(const char *path, turbo_fs_stat_t *stat) {
  if (!path || !stat) {
    return UV_EINVAL;
  }

  uv_fs_t req;
  int err = uv_fs_stat(NULL, &req, path, NULL);

  if (err == 0) {
    // Convert libuv stat to turbo stat
    stat->size = req.statbuf.st_size;
    stat->mode = (int)req.statbuf.st_mode;

    // Convert times to microseconds since epoch
    stat->atime = req.statbuf.st_atim.tv_sec * 1000000ULL + req.statbuf.st_atim.tv_nsec / 1000;
    stat->mtime = req.statbuf.st_mtim.tv_sec * 1000000ULL + req.statbuf.st_mtim.tv_nsec / 1000;
    stat->ctime = req.statbuf.st_ctim.tv_sec * 1000000ULL + req.statbuf.st_ctim.tv_nsec / 1000;

    // Set file type flags
    stat->is_file = S_ISREG(req.statbuf.st_mode);
    stat->is_directory = S_ISDIR(req.statbuf.st_mode);
    stat->is_symlink = S_ISLNK(req.statbuf.st_mode);

    TLOG_DEBUG("Stat sync completed for: {}", path);
  } else {
    TLOG_ERROR("Stat sync failed for {}: {}", path, uv_strerror(err));
  }

  uv_fs_req_cleanup(&req);
  return err;
}

int turbo_fs_mkdir(const char *path, int mode) {
  if (!path) {
    return UV_EINVAL;
  }

  uv_fs_t req;
  int err = uv_fs_mkdir(NULL, &req, path, mode, NULL);

  if (err == 0) {
    TLOG_DEBUG("Directory created: {}", path);
  } else {
    TLOG_ERROR("Failed to create directory {}: {}", path, uv_strerror(err));
  }

  uv_fs_req_cleanup(&req);
  return err;
}

int turbo_fs_rmdir(const char *path) {
  if (!path) {
    return UV_EINVAL;
  }

  uv_fs_t req;
  int err = uv_fs_rmdir(NULL, &req, path, NULL);

  if (err == 0) {
    TLOG_DEBUG("Directory removed: {}", path);
  } else {
    TLOG_ERROR("Failed to remove directory {}: {}", path, uv_strerror(err));
  }

  uv_fs_req_cleanup(&req);
  return err;
}

int turbo_fs_unlink(const char *path) {
  if (!path) {
    return UV_EINVAL;
  }

  uv_fs_t req;
  int err = uv_fs_unlink(NULL, &req, path, NULL);

  if (err == 0) {
    TLOG_DEBUG("File removed: {}", path);
  } else {
    TLOG_ERROR("Failed to remove file {}: {}", path, uv_strerror(err));
  }

  uv_fs_req_cleanup(&req);
  return err;
}

// =============================================================================
// File System Utilities - cross-platform helpers
// =============================================================================

turbo_fs_buf_t turbo_fs_buf_init(char *base, size_t len) {
  turbo_fs_buf_t buf;
  buf.base = base;
  buf.len = len;
  return buf;
}

void turbo_fs_buf_free(turbo_fs_buf_t *buf) {
  if (buf && buf->base) {
    free(buf->base);
    buf->base = NULL;
    buf->len = 0;
  }
}

int turbo_fs_get_tmpdir(char *buffer, size_t buffer_size) {
  if (!buffer || buffer_size == 0) {
    return UV_EINVAL;
  }

  size_t size = buffer_size;
  int err = uv_os_tmpdir(buffer, &size);

  if (err == 0) {
    TLOG_DEBUG("Temporary directory: {}", buffer);
  } else {
    TLOG_ERROR("Failed to get temporary directory: {}", uv_strerror(err));
  }

  return err;
}

// =============================================================================
// Path Utility Functions - cross-platform path operations
// =============================================================================

bool turbo_fs_path_is_absolute(const char *path) {
  if (!path || path[0] == '\0') {
    return false;
  }

#ifdef _WIN32
  // Windows: C:\ or \\server\share
  return (path[0] != '\0' && path[1] == ':' && path[2] == '\\') ||
         (path[0] == '\\' && path[1] == '\\');
#else
  // Unix: starts with /
  return path[0] == '/';
#endif
}

int turbo_fs_path_join(char *result, size_t result_size, const char *base, const char *path) {
  if (!result || !base || !path || result_size == 0) {
    return UV_EINVAL;
  }

  size_t base_len = strlen(base);
  size_t path_len = strlen(path);

  // Check if we need separator
  bool need_separator = false;
  if (base_len > 0) {
#ifdef _WIN32
    char last_char = base[base_len - 1];
    if (last_char != '\\' && last_char != '/') {
      need_separator = true;
    }
#else
    if (base[base_len - 1] != '/') {
      need_separator = true;
    }
#endif
  }

  // Calculate required size
  size_t required = base_len + (need_separator ? 1 : 0) + path_len + 1;
  if (required > result_size) {
    return UV_ENAMETOOLONG;
  }

  // Build path
  strcpy(result, base);
  if (need_separator) {
#ifdef _WIN32
    strcat(result, "\\");
#else
    strcat(result, "/");
#endif
  }
  strcat(result, path);

  return 0;
}

int turbo_fs_path_dirname(const char *path, char *dirname, size_t dirname_size) {
  if (!path || !dirname || dirname_size == 0) {
    return UV_EINVAL;
  }

  size_t path_len = strlen(path);
  if (path_len == 0) {
    if (dirname_size < 2) {
      return UV_ENOMEM;
    }
    strcpy(dirname, ".");
    return 0;
  }

  // Find last separator
  const char *last_sep = NULL;
  for (size_t i = 0; i < path_len; i++) {
#ifdef _WIN32
    if (path[i] == '\\' || path[i] == '/') {
#else
    if (path[i] == '/') {
#endif
      last_sep = &path[i];
    }
  }

  if (!last_sep) {
    // No separator found
    if (dirname_size < 2) {
      return UV_ENOMEM;
    }
    strcpy(dirname, ".");
    return 0;
  }

  // Copy everything before last separator
  size_t dirname_len = last_sep - path;
  if (dirname_len == 0) {
    dirname_len = 1; // Root directory
  }

  if (dirname_len + 1 > dirname_size) {
    return UV_ENAMETOOLONG;
  }

  strncpy(dirname, path, dirname_len);
  dirname[dirname_len] = '\0';

  return 0;
}

int turbo_fs_path_basename(const char *path, char *basename, size_t basename_size) {
  if (!path || !basename || basename_size == 0) {
    return UV_EINVAL;
  }

  size_t path_len = strlen(path);
  if (path_len == 0) {
    if (basename_size < 2) {
      return UV_ENOMEM;
    }
    strcpy(basename, ".");
    return 0;
  }

  // Find last separator
  const char *last_sep = NULL;
  for (size_t i = 0; i < path_len; i++) {
#ifdef _WIN32
    if (path[i] == '\\' || path[i] == '/') {
#else
    if (path[i] == '/') {
#endif
      last_sep = &path[i];
    }
  }

  const char *base_start = last_sep ? last_sep + 1 : path;
  size_t base_len = path_len - (base_start - path);

  if (base_len + 1 > basename_size) {
    return UV_ENAMETOOLONG;
  }

  strcpy(basename, base_start);
  return 0;
}

// =============================================================================
// Streaming File Operations - for incremental I/O
// =============================================================================

static int turbo_fs_flags_to_uv(int flags) {
  int uv_flags = 0;

  if (flags & TURBO_FS_O_RDONLY)
    uv_flags |= UV_FS_O_RDONLY;
  if (flags & TURBO_FS_O_WRONLY)
    uv_flags |= UV_FS_O_WRONLY;
  if (flags & TURBO_FS_O_RDWR)
    uv_flags |= UV_FS_O_RDWR;
  if (flags & TURBO_FS_O_CREAT)
    uv_flags |= UV_FS_O_CREAT;
  if (flags & TURBO_FS_O_TRUNC)
    uv_flags |= UV_FS_O_TRUNC;
  if (flags & TURBO_FS_O_APPEND)
    uv_flags |= UV_FS_O_APPEND;

  return uv_flags;
}

turbo_file_t turbo_fs_open(const char *path, int flags, int mode) {
  if (!path) {
    return TURBO_INVALID_FILE;
  }

  uv_fs_t req;
  int uv_flags = turbo_fs_flags_to_uv(flags);
  int err = uv_fs_open(NULL, &req, path, uv_flags, mode, NULL);

  if (err < 0) {
    uv_fs_req_cleanup(&req);
    return TURBO_INVALID_FILE;
  }

  turbo_file_t fd = (turbo_file_t)req.result;
  return fd;
}

int turbo_fs_ftruncate(turbo_file_t fd, int64_t length) {
  if (fd == TURBO_INVALID_FILE) {
    return UV_EINVAL;
  }

  uv_fs_t req;
  int err = uv_fs_ftruncate(NULL, &req, (uv_file)fd, length, NULL);
  uv_fs_req_cleanup(&req);
  return err;
}

int turbo_fs_read(turbo_file_t fd, char *buf, size_t len) {
  if (fd == TURBO_INVALID_FILE || !buf) {
    return UV_EINVAL;
  }

  uv_fs_t req;
  uv_buf_t uv_buf = uv_buf_init(buf, (unsigned int)len);
  int err = uv_fs_read(NULL, &req, (uv_file)fd, &uv_buf, 1, -1, NULL);

  uv_fs_req_cleanup(&req);
  return err;
}

int turbo_fs_pread(turbo_file_t fd, char *buf, size_t len, int64_t offset) {
  if (fd == TURBO_INVALID_FILE || !buf) {
    return UV_EINVAL;
  }

  uv_fs_t req;
  uv_buf_t uv_buf = uv_buf_init(buf, (unsigned int)len);
  int err = uv_fs_read(NULL, &req, (uv_file)fd, &uv_buf, 1, offset, NULL);

  uv_fs_req_cleanup(&req);
  return err;
}

int turbo_fs_write(turbo_file_t fd, const char *data, size_t len) {
  if (fd == TURBO_INVALID_FILE || !data) {
    return UV_EINVAL;
  }

  uv_fs_t req;
  uv_buf_t buf = uv_buf_init((char *)data, (unsigned int)len);
  int err = uv_fs_write(NULL, &req, (uv_file)fd, &buf, 1, -1, NULL);

  uv_fs_req_cleanup(&req);
  return err;
}

int turbo_fs_close(turbo_file_t fd) {
  if (fd == TURBO_INVALID_FILE) {
    return UV_EINVAL;
  }

  uv_fs_t req;
  int err = uv_fs_close(NULL, &req, (uv_file)fd, NULL);
  uv_fs_req_cleanup(&req);
  return err;
}

int turbo_fs_fsync(turbo_file_t fd) {
  if (fd == TURBO_INVALID_FILE) {
    return UV_EINVAL;
  }

  uv_fs_t req;
  int err = uv_fs_fsync(NULL, &req, (uv_file)fd, NULL);
  uv_fs_req_cleanup(&req);
  return err;
}

int turbo_fs_rename(const char *old_path, const char *new_path) {
  if (!old_path || !new_path) {
    return UV_EINVAL;
  }

  uv_fs_t req;
  int err = uv_fs_rename(NULL, &req, old_path, new_path, NULL);
  uv_fs_req_cleanup(&req);
  return err;
}

int64_t turbo_fs_tell(turbo_file_t fd) {
  if (fd == TURBO_INVALID_FILE) {
    return UV_EINVAL;
  }

  uv_fs_t req;
  uv_buf_t buf = uv_buf_init(NULL, 0);
  // Read 0 bytes at current position to get offset
  int err = uv_fs_read(NULL, &req, (uv_file)fd, &buf, 0, -1, NULL);
  if (err < 0) {
    uv_fs_req_cleanup(&req);
    return err;
  }

  // Use lseek to get current position
  uv_fs_req_cleanup(&req);

  // libuv doesn't have a direct tell, use platform-specific
#ifdef _WIN32
  return _lseeki64(fd, 0, SEEK_CUR);
#else
  return lseek(fd, 0, SEEK_CUR);
#endif
}

int64_t turbo_fs_seek(turbo_file_t fd, int64_t offset, int whence) {
  if (fd == TURBO_INVALID_FILE) {
    return UV_EINVAL;
  }

#ifdef _WIN32
  return _lseeki64(fd, offset, whence);
#else
  return lseek(fd, offset, whence);
#endif
}
