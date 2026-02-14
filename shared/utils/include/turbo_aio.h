/**
 * @file turbo_aio.h
 * @brief Kernel-level Async File I/O (io_uring on Linux, IOCP on Windows)
 *
 * True async I/O without thread pools. Zero-copy where possible.
 */

#ifndef TURBO_AIO_H
#define TURBO_AIO_H

#include <platform.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Types
// =============================================================================

/** Async I/O context (opaque) */
typedef struct turbo_aio_ctx_s turbo_aio_ctx_t;

/** Async operation handle */
typedef struct turbo_aio_op_s turbo_aio_op_t;

/** Operation types */
typedef enum {
    TURBO_AIO_OP_READ = 0,
    TURBO_AIO_OP_WRITE,
    TURBO_AIO_OP_FSYNC,
    TURBO_AIO_OP_OPEN,
    TURBO_AIO_OP_CLOSE,
} turbo_aio_op_type_t;

/** Completion callback */
typedef void (*turbo_aio_cb)(turbo_aio_op_t *op, int result, void *user_data);

/** Operation structure (user-visible part) */
struct turbo_aio_op_s {
    turbo_aio_op_type_t type;
    int fd;
    void *buf;
    size_t len;
    int64_t offset;
    turbo_aio_cb callback;
    void *user_data;
    int result;          /* Bytes transferred or negative error */
    bool completed;
    void *_internal;     /* Platform-specific data */
};

/** Open flags */
#define TURBO_AIO_O_RDONLY   0x0001
#define TURBO_AIO_O_WRONLY   0x0002
#define TURBO_AIO_O_RDWR     0x0004
#define TURBO_AIO_O_CREAT    0x0100
#define TURBO_AIO_O_TRUNC    0x0200
#define TURBO_AIO_O_APPEND   0x0400
#define TURBO_AIO_O_DIRECT   0x1000  /* Bypass page cache */

// =============================================================================
// Context Lifecycle
// =============================================================================

/**
 * @brief Create async I/O context
 * @param queue_depth Maximum pending operations (power of 2 recommended)
 * @return Context or NULL on failure
 */
CXX_C_API turbo_aio_ctx_t *turbo_aio_create(uint32_t queue_depth);

/**
 * @brief Destroy async I/O context
 * @param ctx Context to destroy
 */
CXX_C_API void turbo_aio_destroy(turbo_aio_ctx_t *ctx);

/**
 * @brief Check if true async I/O is available
 * @return true if io_uring/IOCP available, false if fallback to threads
 */
CXX_C_API bool turbo_aio_available(void);

// =============================================================================
// Async Operations
// =============================================================================

/**
 * @brief Submit async read
 * @param ctx Async context
 * @param fd File descriptor
 * @param buf Buffer to read into (must remain valid until completion)
 * @param len Bytes to read
 * @param offset File offset (-1 for current position)
 * @param cb Completion callback
 * @param user_data User data for callback
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_aio_read(turbo_aio_ctx_t *ctx, int fd, void *buf, size_t len,
                             int64_t offset, turbo_aio_cb cb, void *user_data);

/**
 * @brief Submit async write
 * @param ctx Async context
 * @param fd File descriptor
 * @param buf Buffer to write from (must remain valid until completion)
 * @param len Bytes to write
 * @param offset File offset (-1 for current position)
 * @param cb Completion callback
 * @param user_data User data for callback
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_aio_write(turbo_aio_ctx_t *ctx, int fd, const void *buf, size_t len,
                              int64_t offset, turbo_aio_cb cb, void *user_data);

/**
 * @brief Submit async fsync
 * @param ctx Async context
 * @param fd File descriptor
 * @param cb Completion callback
 * @param user_data User data for callback
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_aio_fsync(turbo_aio_ctx_t *ctx, int fd, turbo_aio_cb cb, void *user_data);

/**
 * @brief Submit async open
 * @param ctx Async context
 * @param path File path
 * @param flags Open flags (TURBO_AIO_O_*)
 * @param mode File mode for creation
 * @param cb Completion callback (result is fd or negative error)
 * @param user_data User data for callback
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_aio_open(turbo_aio_ctx_t *ctx, const char *path, int flags, int mode,
                             turbo_aio_cb cb, void *user_data);

/**
 * @brief Submit async close
 * @param ctx Async context
 * @param fd File descriptor
 * @param cb Completion callback
 * @param user_data User data for callback
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_aio_close(turbo_aio_ctx_t *ctx, int fd, turbo_aio_cb cb, void *user_data);

// =============================================================================
// Completion Handling
// =============================================================================

/**
 * @brief Submit all pending operations to kernel
 * @param ctx Async context
 * @return Number of operations submitted, or negative error
 */
CXX_C_API int turbo_aio_submit(turbo_aio_ctx_t *ctx);

/**
 * @brief Wait for and process completions
 * @param ctx Async context
 * @param min_completions Minimum completions to wait for (0 = non-blocking)
 * @param timeout_ms Timeout in milliseconds (-1 = infinite)
 * @return Number of completions processed, or negative error
 */
CXX_C_API int turbo_aio_poll(turbo_aio_ctx_t *ctx, uint32_t min_completions, int timeout_ms);

/**
 * @brief Get number of pending operations
 * @param ctx Async context
 * @return Number of operations in flight
 */
CXX_C_API uint32_t turbo_aio_pending(turbo_aio_ctx_t *ctx);

// =============================================================================
// Error Codes
// =============================================================================

#define TURBO_AIO_OK           0
#define TURBO_AIO_EINVAL      -1
#define TURBO_AIO_ENOMEM      -2
#define TURBO_AIO_ENOSYS      -3   /* Not supported on this platform */
#define TURBO_AIO_EBUSY       -4   /* Queue full */
#define TURBO_AIO_EIO         -5
#define TURBO_AIO_ECANCELED   -6
#define TURBO_AIO_ETIMEDOUT   -7

/**
 * @brief Get error message
 * @param err Error code
 * @return Error message string
 */
CXX_C_API const char *turbo_aio_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_AIO_H */
