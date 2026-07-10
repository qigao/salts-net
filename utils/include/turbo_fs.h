/**
 * @file turboutils_fs.h
 * @brief TurboUtils File System Utilities
 * @author "Simple, direct, no bullshit" - Linus philosophy applied to file I/O
 *
 * This header provides simple file system utilities without exposing libuv.
 * Asynchronous file operations are handled through the main TurboUtils API.
 */

#ifndef turboutils_FS_H
#define turboutils_FS_H

#include "platform.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// File system buffer - compatible with network buffers
typedef struct {
  char *base;
  size_t len;
} turbo_fs_buf_t;

// File information structure
typedef struct {
  uint64_t size;
  uint64_t atime; // Access time (microseconds since epoch)
  uint64_t mtime; // Modify time (microseconds since epoch)
  uint64_t ctime; // Change time (microseconds since epoch)
  int mode;       // File permissions
  bool is_file;
  bool is_directory;
  bool is_symlink;
} turbo_fs_stat_t;

// =============================================================================
// Synchronous File Operations - simple blocking I/O
// =============================================================================

/**
 * @brief Read an entire file
 *
 * This function reads the entire contents of a file into a buffer.
 * Memory is allocated automatically and must be freed with turbo_fs_buf_free().
 *
 * @param path File path to read
 * @param buf Pointer to turbo_fs_buf_t where file contents will be stored
 * @return 0 on success, negative error code on failure
 *
 * @note Blocks until the entire file is read or an error occurs
 * @note Use turbo_fs_buf_free() to release the buffer when done
 */
CXX_C_API int turbo_fs_read_file(const char *path, turbo_fs_buf_t *buf);

/**
 * @brief Write data to a file
 *
 * This function writes a buffer of data to a file, creating it if necessary.
 *
 * @param path File path to write to
 * @param buf Pointer to turbo_fs_buf_t containing data to write
 * @return 0 on success, negative error code on failure
 *
 * @note Blocks until the entire buffer is written or an error occurs
 * @note Creates the file if it doesn't exist, overwrites if it does
 */
CXX_C_API int turbo_fs_write_file(const char *path, const turbo_fs_buf_t *buf);

/**
 * @brief Get file information
 *
 * This function retrieves metadata about a file or directory.
 *
 * @param path Path to file or directory to stat
 * @param stat Pointer to turbo_fs_stat_t to fill with file information
 * @return 0 on success, negative error code on failure
 *
 * @note Works on both files and directories
 * @note Provides size, permissions, timestamps, and file type information
 */
CXX_C_API int turbo_fs_stat(const char *path, turbo_fs_stat_t *stat);

/**
 * @brief Get file information without following symbolic links when supported
 *
 * @param path Path to file, directory, or symbolic link
 * @param stat Pointer to turbo_fs_stat_t to fill with file information
 * @return 0 on success, negative error code on failure
 *
 * @note On POSIX this uses lstat(). On Windows this reports reparse-point
 *       symlinks using Win32 file attributes.
 */
CXX_C_API int turbo_fs_lstat(const char *path, turbo_fs_stat_t *stat);

/**
 * @brief Change file permissions
 *
 * @param path Path to file
 * @param mode Permission bits, e.g. 0644
 * @return 0 on success, negative error code on failure
 *
 * @note Windows maps this to read-only/read-write CRT permissions.
 */
CXX_C_API int turbo_fs_chmod(const char *path, int mode);

// Access check flags for turbo_fs_access()
#define TURBO_FS_ACCESS_EXISTS 0x00
#define TURBO_FS_ACCESS_READ   0x01
#define TURBO_FS_ACCESS_WRITE  0x02
#define TURBO_FS_ACCESS_EXEC   0x04

/**
 * @brief Check file accessibility
 *
 * @param path Path to check
 * @param mode TURBO_FS_ACCESS_* bitmask, or TURBO_FS_ACCESS_EXISTS
 * @return 0 if accessible, negative error code otherwise
 *
 * @note Windows CRT access checks read/write/existence. Execute is treated as
 *       existence on Windows because executable permission is not a mode bit.
 */
CXX_C_API int turbo_fs_access(const char *path, int mode);

/**
 * @brief Create a symbolic link
 *
 * @param target Link target path
 * @param link_path Path of the symlink to create
 * @param is_directory Non-zero when target is a directory
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_fs_symlink(const char *target, const char *link_path, int is_directory);

/**
 * @brief Read a symbolic link target into buffer
 *
 * @param path Symlink path
 * @param buffer Destination buffer
 * @param buffer_size Destination size including trailing NUL
 * @return Number of bytes written, or negative error code on failure
 *
 * @note POSIX returns the symlink payload. Windows returns the resolved final
 *       path for the reparse point because Win32 does not expose POSIX readlink
 *       semantics through the CRT.
 */
CXX_C_API int turbo_fs_readlink(const char *path, char *buffer, size_t buffer_size);

/**
 * @brief Create a directory
 *
 * @param path Directory path to create
 * @param mode Directory permissions (e.g., 0755)
 * @return 0 on success, negative error code on failure
 *
 * @note Parent directories must exist
 * @note Permissions are ignored on Windows platforms
 */
CXX_C_API int turbo_fs_mkdir(const char *path, int mode);

/**
 * @brief Remove a directory
 *
 * @param path Directory path to remove
 * @return 0 on success, negative error code on failure
 *
 * @note Directory must be empty
 */
CXX_C_API int turbo_fs_rmdir(const char *path);

/**
 * @brief Delete a file
 *
 * @param path File path to delete
 * @return 0 on success, negative error code on failure
 *
 * @note Only works on files, not directories
 */
CXX_C_API int turbo_fs_unlink(const char *path);

// =============================================================================
// File System Utilities - cross-platform helpers
// =============================================================================

/**
 * @brief Initialize a file system buffer
 *
 * @param base Pointer to the buffer memory
 * @param len Length of the buffer in bytes
 * @return Initialized turbo_fs_buf_t structure
 */
CXX_C_API turbo_fs_buf_t turbo_fs_buf_init(char *base, size_t len);

/**
 * @brief Free a file system buffer allocated by TurboUtils
 *
 * @param buf Pointer to turbo_fs_buf_t to free
 *
 * @note Only call this on buffers allocated by TurboUtils functions
 */
CXX_C_API void turbo_fs_buf_free(turbo_fs_buf_t *buf);

/**
 * @brief Get temporary directory path (cross-platform)
 *
 * @param buffer Buffer to store the directory path
 * @param buffer_size Size of the buffer in bytes
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_fs_get_tmpdir(char *buffer, size_t buffer_size);

/**
 * @brief Check if a path is absolute
 *
 * @param path Path to check
 * @return true if path is absolute, false if relative
 */
CXX_C_API bool turbo_fs_path_is_absolute(const char *path);

/**
 * @brief Join two path components into a single path
 *
 * @param result Buffer to store the joined path
 * @param result_size Size of the result buffer in bytes
 * @param base Base path component
 * @param path Relative path component to append
 * @return 0 on success, -1 if result buffer is too small
 */
CXX_C_API int turbo_fs_path_join(char *result, size_t result_size, const char *base,
                                 const char *path);

/**
 * @brief Extract directory component from a path
 *
 * @param path Original path
 * @param dirname Buffer to store the directory component
 * @param dirname_size Size of the dirname buffer in bytes
 * @return 0 on success, -1 if dirname buffer is too small
 */
CXX_C_API int turbo_fs_path_dirname(const char *path, char *dirname, size_t dirname_size);

/**
 * @brief Extract filename component from a path
 *
 * @param path Original path
 * @param basename Buffer to store the filename component
 * @param basename_size Size of the basename buffer in bytes
 * @return 0 on success, -1 if basename buffer is too small
 */
CXX_C_API int turbo_fs_path_basename(const char *path, char *basename, size_t basename_size);

// File system constants
#define TURBO_FS_MAX_PATH 260
#define TURBO_FS_DEFAULT_MODE 0644

// File open flags
#define TURBO_FS_O_RDONLY 0x0001
#define TURBO_FS_O_WRONLY 0x0002
#define TURBO_FS_O_RDWR 0x0004
#define TURBO_FS_O_CREAT 0x0100
#define TURBO_FS_O_TRUNC 0x0200
#define TURBO_FS_O_APPEND 0x0400

// Advisory file lock flags for turbo_fs_lock()
#define TURBO_FS_LOCK_SHARED    0x01
#define TURBO_FS_LOCK_EXCLUSIVE 0x02
#define TURBO_FS_LOCK_NONBLOCK  0x04

// File handle type
typedef int turbo_file_t;
#define TURBO_INVALID_FILE (-1)

// =============================================================================
// Streaming File Operations - for incremental I/O
// =============================================================================

/**
 * @brief Open a file
 *
 * @param path File path to open
 * @param flags Open flags (TURBO_FS_O_*)
 * @param mode File permissions for new files (e.g., 0644)
 * @return File handle on success, TURBO_INVALID_FILE on failure
 */
CXX_C_API turbo_file_t turbo_fs_open(const char *path, int flags, int mode);

/**
 * @brief Read from an open file
 *
 * @param fd File handle from turbo_fs_open
 * @param buf Buffer to store read data
 * @param len Maximum number of bytes to read
 * @return Number of bytes read, or negative error code
 *
 * @note len must be <= INT_MAX because the return type is int
 */
CXX_C_API int turbo_fs_read(turbo_file_t fd, char *buf, size_t len);

/**
 * @brief Read from an open file at a specific offset (thread-safe)
 *
 * Uses positional read — does not modify the file descriptor's position.
 * Safe to call concurrently from multiple threads on the same fd.
 *
 * @param fd File handle from turbo_fs_open
 * @param buf Buffer to store read data
 * @param len Maximum number of bytes to read
 * @param offset Byte offset in the file to read from
 * @return Number of bytes read, or negative error code
 *
 * @note len must be <= INT_MAX because the return type is int
 */
CXX_C_API int turbo_fs_pread(turbo_file_t fd, char *buf, size_t len, int64_t offset);

/**
 * @brief Write to an open file at a specific offset
 *
 * @param fd File handle from turbo_fs_open
 * @param data Data to write
 * @param len Number of bytes to write
 * @param offset Byte offset in the file to write to
 * @return Number of bytes written, or negative error code
 *
 * @note len must be <= INT_MAX because the return type is int
 */
CXX_C_API int turbo_fs_pwrite(turbo_file_t fd, const char *data, size_t len, int64_t offset);

/**
 * @brief Write to an open file
 *
 * @param fd File handle from turbo_fs_open
 * @param data Data to write
 * @param len Number of bytes to write
 * @return Number of bytes written, or negative error code
 *
 * @note len must be <= INT_MAX because the return type is int
 */
CXX_C_API int turbo_fs_write(turbo_file_t fd, const char *data, size_t len);

/**
 * @brief Close an open file
 *
 * @param fd File handle to close
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_fs_close(turbo_file_t fd);

/**
 * @brief Truncate or extend a file to a specified length
 *
 * @param fd File handle
 * @param length New length in bytes
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_fs_ftruncate(turbo_file_t fd, int64_t length);

/**
 * @brief Flush file buffers to disk
 *
 * @param fd File handle to flush
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_fs_fsync(turbo_file_t fd);

/**
 * @brief Acquire an advisory byte-range lock
 *
 * @param fd File handle
 * @param flags TURBO_FS_LOCK_SHARED or TURBO_FS_LOCK_EXCLUSIVE, optionally
 *              OR'd with TURBO_FS_LOCK_NONBLOCK
 * @param offset Start offset
 * @param len Length to lock; 0 means to EOF
 * @return 0 on success, negative error code on failure
 *
 * @note POSIX uses fcntl record locks. Windows uses LockFileEx. Locks are
 *       advisory and process/OS semantics differ across platforms.
 */
CXX_C_API int turbo_fs_lock(turbo_file_t fd, int flags, int64_t offset, uint64_t len);

/**
 * @brief Release an advisory byte-range lock
 *
 * @param fd File handle
 * @param offset Start offset
 * @param len Length to unlock; 0 means to EOF
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_fs_unlock(turbo_file_t fd, int64_t offset, uint64_t len);

/**
 * @brief Rename/move a file
 *
 * @param old_path Current file path
 * @param new_path New file path
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_fs_rename(const char *old_path, const char *new_path);

/**
 * @brief Get current file position
 *
 * @param fd File handle
 * @return Current position, or negative error code
 */
CXX_C_API int64_t turbo_fs_tell(turbo_file_t fd);

/**
 * @brief Seek to position in file
 *
 * @param fd File handle
 * @param offset Offset from whence
 * @param whence SEEK_SET, SEEK_CUR, or SEEK_END
 * @return New position, or negative error code
 */
CXX_C_API int64_t turbo_fs_seek(turbo_file_t fd, int64_t offset, int whence);

#ifdef __cplusplus
}
#endif

#endif // turboutils_FS_H
