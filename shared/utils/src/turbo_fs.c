/**
 * @file turbo_fs.c
 * @brief TurboNet File System - Synchronous cross-platform I/O, zero dependencies.
 *
 * "Simple, direct, no bullshit."
 * No libuv. Just standard POSIX / Win32 syscalls.
 * Error codes: negative errno values (e.g. -EINVAL, -ENOMEM), mirroring POSIX convention.
 */
#include "platform.h"

#include "tlog.h"
#include "turbo_fs.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ── Platform headers ─────────────────────────────────────────────────────── */
#ifdef _WIN32
  #include <direct.h>
  #include <fcntl.h>
  #include <io.h>
  #include <sys/stat.h>
  #include <sys/types.h>

  /* POSIX-style open/read/write/close live in <io.h> on MSVC */
  #define fs_open    _open
  #define fs_read    _read
  #define fs_write   _write
  #define fs_close   _close
  #define fs_lseek   _lseeki64
  #define fs_fstat   _fstat64
  #define fs_stat_t  struct __stat64
  #define fs_ftrunc  _chsize_s
  #define fs_fsync   _commit

  /* Win32 does not have S_ISxxx macros by default */
  #ifndef S_ISREG
    #define S_ISREG(m)  (((m) & _S_IFMT) == _S_IFREG)
  #endif
  #ifndef S_ISDIR
    #define S_ISDIR(m)  (((m) & _S_IFMT) == _S_IFDIR)
  #endif
  #ifndef S_ISLNK
    #define S_ISLNK(m)  0
  #endif

  /* Time fields: MSVC uses st_atime (time_t), not st_atim (timespec) */
  #define FS_ST_ATIME_SEC(s)  ((s).st_atime)
  #define FS_ST_MTIME_SEC(s)  ((s).st_mtime)
  #define FS_ST_CTIME_SEC(s)  ((s).st_ctime)
  #define FS_ST_XTIME_NSEC    0   /* MSVC stat has no nanosecond field */

#else  /* POSIX */
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>

  #define fs_open    open
  #define fs_read    read
  #define fs_write   write
  #define fs_close   close
  #define fs_lseek   lseek
  #define fs_fstat   fstat
  #define fs_stat_t  struct stat
  #define fs_ftrunc  ftruncate
  #define fs_fsync   fsync

  #define FS_ST_ATIME_SEC(s)  ((s).st_atim.tv_sec)
  #define FS_ST_MTIME_SEC(s)  ((s).st_mtim.tv_sec)
  #define FS_ST_CTIME_SEC(s)  ((s).st_ctim.tv_sec)

  /* nanosecond sub-field for conversion to microseconds */
  #define FS_ST_ATIME_NSEC(s) ((s).st_atim.tv_nsec)
  #define FS_ST_MTIME_NSEC(s) ((s).st_mtim.tv_nsec)
  #define FS_ST_CTIME_NSEC(s) ((s).st_ctim.tv_nsec)
#endif

/* Helper: map errno to negative return value */
static inline int err_from_errno(void) { return -errno; }

/* ── Flag translation: TURBO_FS_O_* → native O_* ─────────────────────────── */
static int turbo_fs_flags_to_native(int flags) {
  int native = 0;

  /* Access mode: exactly one of RDONLY / WRONLY / RDWR must be set */
  if (flags & TURBO_FS_O_RDWR)
    native |= O_RDWR;
  else if (flags & TURBO_FS_O_WRONLY)
    native |= O_WRONLY;
  else
    native |= O_RDONLY;

  if (flags & TURBO_FS_O_CREAT)
    native |= O_CREAT;
  if (flags & TURBO_FS_O_TRUNC)
    native |= O_TRUNC;
  if (flags & TURBO_FS_O_APPEND)
    native |= O_APPEND;

#ifdef _WIN32
  /* Windows: always open in binary mode to avoid CRLF surprises */
  native |= O_BINARY;
#endif

  return native;
}

/* ── Synchronous File Operations ─────────────────────────────────────────── */

int turbo_fs_read_file(const char *path, turbo_fs_buf_t *buf) {
  if (!path || !buf) {
    return -EINVAL;
  }

  /* stat to get size */
  fs_stat_t st;
#ifdef _WIN32
  if (_stat64(path, &st) != 0) {
#else
  if (stat(path, &st) != 0) {
#endif
    int e = err_from_errno();
    TLOG_ERROR("Failed to stat file {}: {}", path, strerror(-e));
    return e;
  }

  size_t file_size = (size_t)st.st_size;

  /* open */
  int fd = fs_open(path, O_RDONLY
#ifdef _WIN32
                   | O_BINARY
#endif
                   , 0);
  if (fd < 0) {
    int e = err_from_errno();
    TLOG_ERROR("Failed to open file {}: {}", path, strerror(-e));
    return e;
  }

  /* allocate */
  buf->base = malloc(file_size + 1);
  if (!buf->base) {
    fs_close(fd);
    return -ENOMEM;
  }
  buf->len = file_size;

  /* read */
  ssize_t n = fs_read(fd, buf->base, (unsigned int)file_size);
  fs_close(fd);

  if (n < 0) {
    int e = err_from_errno();
    TLOG_ERROR("Failed to read file {}: {}", path, strerror(-e));
    free(buf->base);
    buf->base = NULL;
    buf->len  = 0;
    return e;
  }

  buf->len         = (size_t)n;
  buf->base[n]     = '\0';
  TLOG_DEBUG("Read file {}: {} bytes", path, buf->len);
  return 0;
}

int turbo_fs_write_file(const char *path, const turbo_fs_buf_t *buf) {
  if (!path || !buf || !buf->base) {
    return -EINVAL;
  }

  int fd = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC
#ifdef _WIN32
                   | O_BINARY
#endif
                   , TURBO_FS_DEFAULT_MODE);
  if (fd < 0) {
    int e = err_from_errno();
    TLOG_ERROR("Failed to open file {}: {}", path, strerror(-e));
    return e;
  }

  ssize_t n = fs_write(fd, buf->base, (unsigned int)buf->len);
  fs_close(fd);

  if (n < 0) {
    int e = err_from_errno();
    TLOG_ERROR("Failed to write file {}: {}", path, strerror(-e));
    return e;
  }

  TLOG_DEBUG("Wrote file {}: {} bytes", path, buf->len);
  return 0;
}

int turbo_fs_stat(const char *path, turbo_fs_stat_t *stat_out) {
  if (!path || !stat_out) {
    return -EINVAL;
  }

  fs_stat_t st;
#ifdef _WIN32
  if (_stat64(path, &st) != 0) {
#else
  if (stat(path, &st) != 0) {
#endif
    int e = err_from_errno();
    TLOG_ERROR("Stat failed for {}: {}", path, strerror(-e));
    return e;
  }

  stat_out->size        = (uint64_t)st.st_size;
  stat_out->mode        = (int)st.st_mode;
  stat_out->is_file      = S_ISREG(st.st_mode)  ? true : false;
  stat_out->is_directory = S_ISDIR(st.st_mode)  ? true : false;
  stat_out->is_symlink   = S_ISLNK(st.st_mode)  ? true : false;

#ifdef _WIN32
  /* MSVC: only second-resolution; no nanosecond field */
  stat_out->atime = (uint64_t)st.st_atime * 1000000ULL;
  stat_out->mtime = (uint64_t)st.st_mtime * 1000000ULL;
  stat_out->ctime = (uint64_t)st.st_ctime * 1000000ULL;
#else
  stat_out->atime = (uint64_t)FS_ST_ATIME_SEC(st) * 1000000ULL + (uint64_t)FS_ST_ATIME_NSEC(st) / 1000;
  stat_out->mtime = (uint64_t)FS_ST_MTIME_SEC(st) * 1000000ULL + (uint64_t)FS_ST_MTIME_NSEC(st) / 1000;
  stat_out->ctime = (uint64_t)FS_ST_CTIME_SEC(st) * 1000000ULL + (uint64_t)FS_ST_CTIME_NSEC(st) / 1000;
#endif

  TLOG_DEBUG("Stat completed for: {}", path);
  return 0;
}

int turbo_fs_mkdir(const char *path, int mode) {
  if (!path) {
    return -EINVAL;
  }

#ifdef _WIN32
  int err = _mkdir(path);
#else
  int err = mkdir(path, (mode_t)mode);
#endif

  if (err != 0) {
    int e = err_from_errno();
    TLOG_ERROR("Failed to create directory {}: {}", path, strerror(-e));
    return e;
  }

  TLOG_DEBUG("Directory created: {}", path);
  return 0;
}

int turbo_fs_rmdir(const char *path) {
  if (!path) {
    return -EINVAL;
  }

#ifdef _WIN32
  int err = _rmdir(path);
#else
  int err = rmdir(path);
#endif

  if (err != 0) {
    int e = err_from_errno();
    TLOG_ERROR("Failed to remove directory {}: {}", path, strerror(-e));
    return e;
  }

  TLOG_DEBUG("Directory removed: {}", path);
  return 0;
}

int turbo_fs_unlink(const char *path) {
  if (!path) {
    return -EINVAL;
  }

  if (remove(path) != 0) {
    int e = err_from_errno();
    TLOG_ERROR("Failed to remove file {}: {}", path, strerror(-e));
    return e;
  }

  TLOG_DEBUG("File removed: {}", path);
  return 0;
}

/* ── Buffer Utilities ────────────────────────────────────────────────────── */

turbo_fs_buf_t turbo_fs_buf_init(char *base, size_t len) {
  turbo_fs_buf_t buf;
  buf.base = base;
  buf.len  = len;
  return buf;
}

void turbo_fs_buf_free(turbo_fs_buf_t *buf) {
  if (buf && buf->base) {
    free(buf->base);
    buf->base = NULL;
    buf->len  = 0;
  }
}

int turbo_fs_get_tmpdir(char *buffer, size_t buffer_size) {
  if (!buffer || buffer_size == 0) {
    return -EINVAL;
  }

#ifdef _WIN32
  DWORD n = GetTempPathA((DWORD)buffer_size, buffer);
  if (n == 0 || n >= buffer_size) {
    TLOG_ERROR("Failed to get temporary directory");
    return -ERANGE;
  }
  /* Strip trailing backslash for consistency */
  if (n > 1 && buffer[n - 1] == '\\') {
    buffer[n - 1] = '\0';
  }
#else
  const char *tmp = getenv("TMPDIR");
  if (!tmp) tmp = getenv("TMP");
  if (!tmp) tmp = getenv("TEMP");
  if (!tmp) tmp = "/tmp";

  size_t len = strlen(tmp);
  if (len >= buffer_size) {
    TLOG_ERROR("Failed to get temporary directory: buffer too small");
    return -ERANGE;
  }
  memcpy(buffer, tmp, len + 1);
#endif

  TLOG_DEBUG("Temporary directory: {}", buffer);
  return 0;
}

/* ── Path Utilities ──────────────────────────────────────────────────────── */

bool turbo_fs_path_is_absolute(const char *path) {
  if (!path || path[0] == '\0') {
    return false;
  }
#ifdef _WIN32
  return (path[1] == ':' && (path[2] == '\\' || path[2] == '/')) ||
         (path[0] == '\\' && path[1] == '\\');
#else
  return path[0] == '/';
#endif
}

int turbo_fs_path_join(char *result, size_t result_size, const char *base, const char *path) {
  if (!result || !base || !path || result_size == 0) {
    return -EINVAL;
  }

  size_t base_len = strlen(base);
  size_t path_len = strlen(path);
  bool need_sep   = false;

  if (base_len > 0) {
    char last = base[base_len - 1];
#ifdef _WIN32
    need_sep = (last != '\\' && last != '/');
#else
    need_sep = (last != '/');
#endif
  }

  size_t required = base_len + (need_sep ? 1 : 0) + path_len + 1;
  if (required > result_size) {
    return -ENAMETOOLONG;
  }

  strcpy(result, base);
  if (need_sep) {
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
    return -EINVAL;
  }

  size_t path_len = strlen(path);
  if (path_len == 0) {
    if (dirname_size < 2) return -ENOMEM;
    strcpy(dirname, ".");
    return 0;
  }

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
    if (dirname_size < 2) return -ENOMEM;
    strcpy(dirname, ".");
    return 0;
  }

  size_t dlen = (size_t)(last_sep - path);
  if (dlen == 0) dlen = 1; /* root: "/" or "\" */

  if (dlen + 1 > dirname_size) return -ENAMETOOLONG;

  strncpy(dirname, path, dlen);
  dirname[dlen] = '\0';
  return 0;
}

int turbo_fs_path_basename(const char *path, char *basename, size_t basename_size) {
  if (!path || !basename || basename_size == 0) {
    return -EINVAL;
  }

  size_t path_len = strlen(path);
  if (path_len == 0) {
    if (basename_size < 2) return -ENOMEM;
    strcpy(basename, ".");
    return 0;
  }

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
  size_t      base_len   = path_len - (size_t)(base_start - path);

  if (base_len + 1 > basename_size) return -ENAMETOOLONG;

  strcpy(basename, base_start);
  return 0;
}

/* ── Streaming File Operations ───────────────────────────────────────────── */

turbo_file_t turbo_fs_open(const char *path, int flags, int mode) {
  if (!path) {
    return TURBO_INVALID_FILE;
  }

  int native_flags = turbo_fs_flags_to_native(flags);
  int fd           = fs_open(path, native_flags, mode);

  if (fd < 0) {
    TLOG_ERROR("Failed to open file {}: {}", path, strerror(errno));
    return TURBO_INVALID_FILE;
  }

  return (turbo_file_t)fd;
}

int turbo_fs_ftruncate(turbo_file_t fd, int64_t length) {
  if (fd == TURBO_INVALID_FILE) {
    return -EINVAL;
  }

  if (fs_ftrunc(fd, length) != 0) {
    return err_from_errno();
  }
  return 0;
}

int turbo_fs_read(turbo_file_t fd, char *buf, size_t len) {
  if (fd == TURBO_INVALID_FILE || !buf) {
    return -EINVAL;
  }

  ssize_t n = fs_read(fd, buf, (unsigned int)len);
  if (n < 0) {
    return err_from_errno();
  }
  return (int)n;
}

int turbo_fs_pread(turbo_file_t fd, char *buf, size_t len, int64_t offset) {
  if (fd == TURBO_INVALID_FILE || !buf) {
    return -EINVAL;
  }

#ifdef _WIN32
  /* pread emulation: save position, seek, read, restore */
  int64_t saved = _lseeki64(fd, 0, SEEK_CUR);
  if (saved < 0) return err_from_errno();
  if (_lseeki64(fd, offset, SEEK_SET) < 0) return err_from_errno();
  ssize_t n = _read(fd, buf, (unsigned int)len);
  _lseeki64(fd, saved, SEEK_SET);
#else
  ssize_t n = pread(fd, buf, len, (off_t)offset);
#endif

  if (n < 0) return err_from_errno();
  return (int)n;
}

int turbo_fs_pwrite(turbo_file_t fd, const char *data, size_t len, int64_t offset) {
  if (fd == TURBO_INVALID_FILE || !data) {
    return -EINVAL;
  }

#ifdef _WIN32
  int64_t saved = _lseeki64(fd, 0, SEEK_CUR);
  if (saved < 0) return err_from_errno();
  if (_lseeki64(fd, offset, SEEK_SET) < 0) return err_from_errno();
  ssize_t n = _write(fd, data, (unsigned int)len);
  _lseeki64(fd, saved, SEEK_SET);
#else
  ssize_t n = pwrite(fd, data, len, (off_t)offset);
#endif

  if (n < 0) return err_from_errno();
  return (int)n;
}

int turbo_fs_write(turbo_file_t fd, const char *data, size_t len) {
  if (fd == TURBO_INVALID_FILE || !data) {
    return -EINVAL;
  }

  ssize_t n = fs_write(fd, data, (unsigned int)len);
  if (n < 0) return err_from_errno();
  return (int)n;
}

int turbo_fs_close(turbo_file_t fd) {
  if (fd == TURBO_INVALID_FILE) {
    return -EINVAL;
  }

  if (fs_close(fd) != 0) {
    return err_from_errno();
  }
  return 0;
}

int turbo_fs_fsync(turbo_file_t fd) {
  if (fd == TURBO_INVALID_FILE) {
    return -EINVAL;
  }

  if (fs_fsync(fd) != 0) {
    return err_from_errno();
  }
  return 0;
}

int turbo_fs_rename(const char *old_path, const char *new_path) {
  if (!old_path || !new_path) {
    return -EINVAL;
  }

  if (rename(old_path, new_path) != 0) {
    return err_from_errno();
  }
  return 0;
}

int64_t turbo_fs_tell(turbo_file_t fd) {
  if (fd == TURBO_INVALID_FILE) {
    return -EINVAL;
  }

  int64_t pos = fs_lseek(fd, 0, SEEK_CUR);
  if (pos < 0) return err_from_errno();
  return pos;
}

int64_t turbo_fs_seek(turbo_file_t fd, int64_t offset, int whence) {
  if (fd == TURBO_INVALID_FILE) {
    return -EINVAL;
  }

  int64_t pos = fs_lseek(fd, offset, whence);
  if (pos < 0) return err_from_errno();
  return pos;
}
