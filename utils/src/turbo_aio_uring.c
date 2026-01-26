/**
 * @file turbo_aio_uring.c
 * @brief Linux io_uring implementation for async file I/O
 *
 * Requires Linux 5.1+ with liburing.
 */

#include "turbo_aio.h"

#ifdef __linux__

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// =============================================================================
// Internal Structures
// =============================================================================

typedef struct {
    turbo_aio_op_t op;
    char *path;  /* For open operations */
} aio_request_t;

struct turbo_aio_ctx_s {
    struct io_uring ring;
    uint32_t queue_depth;
    uint32_t pending;
    bool initialized;
};

// =============================================================================
// Helpers
// =============================================================================

static int flags_to_linux(int flags) {
    int result = 0;
    if (flags & TURBO_AIO_O_RDONLY) result |= O_RDONLY;
    if (flags & TURBO_AIO_O_WRONLY) result |= O_WRONLY;
    if (flags & TURBO_AIO_O_RDWR)   result |= O_RDWR;
    if (flags & TURBO_AIO_O_CREAT)  result |= O_CREAT;
    if (flags & TURBO_AIO_O_TRUNC)  result |= O_TRUNC;
    if (flags & TURBO_AIO_O_APPEND) result |= O_APPEND;
#ifdef O_DIRECT
    if (flags & TURBO_AIO_O_DIRECT) result |= O_DIRECT;
#endif
    return result;
}

static aio_request_t *alloc_request(turbo_aio_op_type_t type, int fd,
                                    void *buf, size_t len, int64_t offset,
                                    turbo_aio_cb cb, void *user_data) {
    aio_request_t *req = calloc(1, sizeof(aio_request_t));
    if (!req) return NULL;
    req->op.type = type;
    req->op.fd = fd;
    req->op.buf = buf;
    req->op.len = len;
    req->op.offset = offset;
    req->op.callback = cb;
    req->op.user_data = user_data;
    req->op.completed = false;
    return req;
}

static void free_request(aio_request_t *req) {
    if (req) {
        if (req->path) free(req->path);
        free(req);
    }
}

// =============================================================================
// Context Lifecycle
// =============================================================================

CXX_C_API bool turbo_aio_available(void) {
    struct io_uring ring;
    int ret = io_uring_queue_init(1, &ring, 0);
    if (ret == 0) {
        io_uring_queue_exit(&ring);
        return true;
    }
    return false;
}

CXX_C_API turbo_aio_ctx_t *turbo_aio_create(uint32_t queue_depth) {
    turbo_aio_ctx_t *ctx = calloc(1, sizeof(turbo_aio_ctx_t));
    if (!ctx) return NULL;

    int ret = io_uring_queue_init(queue_depth, &ctx->ring, 0);
    if (ret < 0) {
        free(ctx);
        return NULL;
    }

    ctx->queue_depth = queue_depth;
    ctx->pending = 0;
    ctx->initialized = true;
    return ctx;
}

CXX_C_API void turbo_aio_destroy(turbo_aio_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->initialized) {
        io_uring_queue_exit(&ctx->ring);
    }
    free(ctx);
}

// =============================================================================
// Async Operations
// =============================================================================

CXX_C_API int turbo_aio_read(turbo_aio_ctx_t *ctx, int fd, void *buf, size_t len,
                             int64_t offset, turbo_aio_cb cb, void *user_data) {
    if (!ctx || !buf || fd < 0) return TURBO_AIO_EINVAL;

    aio_request_t *req = alloc_request(TURBO_AIO_OP_READ, fd, buf, len, offset, cb, user_data);
    if (!req) return TURBO_AIO_ENOMEM;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        free_request(req);
        return TURBO_AIO_EBUSY;
    }

    io_uring_prep_read(sqe, fd, buf, len, offset >= 0 ? offset : -1);
    io_uring_sqe_set_data(sqe, req);
    ctx->pending++;
    return TURBO_AIO_OK;
}

CXX_C_API int turbo_aio_write(turbo_aio_ctx_t *ctx, int fd, const void *buf, size_t len,
                              int64_t offset, turbo_aio_cb cb, void *user_data) {
    if (!ctx || !buf || fd < 0) return TURBO_AIO_EINVAL;

    aio_request_t *req = alloc_request(TURBO_AIO_OP_WRITE, fd, (void *)buf, len, offset, cb, user_data);
    if (!req) return TURBO_AIO_ENOMEM;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        free_request(req);
        return TURBO_AIO_EBUSY;
    }

    io_uring_prep_write(sqe, fd, buf, len, offset >= 0 ? offset : -1);
    io_uring_sqe_set_data(sqe, req);
    ctx->pending++;
    return TURBO_AIO_OK;
}

CXX_C_API int turbo_aio_fsync(turbo_aio_ctx_t *ctx, int fd, turbo_aio_cb cb, void *user_data) {
    if (!ctx || fd < 0) return TURBO_AIO_EINVAL;

    aio_request_t *req = alloc_request(TURBO_AIO_OP_FSYNC, fd, NULL, 0, 0, cb, user_data);
    if (!req) return TURBO_AIO_ENOMEM;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        free_request(req);
        return TURBO_AIO_EBUSY;
    }

    io_uring_prep_fsync(sqe, fd, 0);
    io_uring_sqe_set_data(sqe, req);
    ctx->pending++;
    return TURBO_AIO_OK;
}

CXX_C_API int turbo_aio_open(turbo_aio_ctx_t *ctx, const char *path, int flags, int mode,
                             turbo_aio_cb cb, void *user_data) {
    if (!ctx || !path) return TURBO_AIO_EINVAL;

    aio_request_t *req = alloc_request(TURBO_AIO_OP_OPEN, -1, NULL, 0, 0, cb, user_data);
    if (!req) return TURBO_AIO_ENOMEM;

    req->path = strdup(path);
    if (!req->path) {
        free_request(req);
        return TURBO_AIO_ENOMEM;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        free_request(req);
        return TURBO_AIO_EBUSY;
    }

    io_uring_prep_openat(sqe, AT_FDCWD, req->path, flags_to_linux(flags), mode);
    io_uring_sqe_set_data(sqe, req);
    ctx->pending++;
    return TURBO_AIO_OK;
}

CXX_C_API int turbo_aio_close(turbo_aio_ctx_t *ctx, int fd, turbo_aio_cb cb, void *user_data) {
    if (!ctx || fd < 0) return TURBO_AIO_EINVAL;

    aio_request_t *req = alloc_request(TURBO_AIO_OP_CLOSE, fd, NULL, 0, 0, cb, user_data);
    if (!req) return TURBO_AIO_ENOMEM;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        free_request(req);
        return TURBO_AIO_EBUSY;
    }

    io_uring_prep_close(sqe, fd);
    io_uring_sqe_set_data(sqe, req);
    ctx->pending++;
    return TURBO_AIO_OK;
}

// =============================================================================
// Completion Handling
// =============================================================================

CXX_C_API int turbo_aio_submit(turbo_aio_ctx_t *ctx) {
    if (!ctx) return TURBO_AIO_EINVAL;
    int ret = io_uring_submit(&ctx->ring);
    return ret < 0 ? TURBO_AIO_EIO : ret;
}

CXX_C_API int turbo_aio_poll(turbo_aio_ctx_t *ctx, uint32_t min_completions, int timeout_ms) {
    if (!ctx) return TURBO_AIO_EINVAL;

    struct io_uring_cqe *cqe;
    int completed = 0;

    if (min_completions == 0) {
        /* Non-blocking peek */
        while (io_uring_peek_cqe(&ctx->ring, &cqe) == 0) {
            aio_request_t *req = io_uring_cqe_get_data(cqe);
            if (req) {
                req->op.result = cqe->res;
                req->op.completed = true;
                if (req->op.callback) {
                    req->op.callback(&req->op, cqe->res, req->op.user_data);
                }
                free_request(req);
                ctx->pending--;
            }
            io_uring_cqe_seen(&ctx->ring, cqe);
            completed++;
        }
    } else {
        /* Blocking wait */
        struct __kernel_timespec ts;
        struct __kernel_timespec *pts = NULL;
        if (timeout_ms >= 0) {
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000;
            pts = &ts;
        }

        int ret = io_uring_wait_cqe_timeout(&ctx->ring, &cqe, pts);
        if (ret == -ETIME) return 0;
        if (ret < 0) return TURBO_AIO_EIO;

        do {
            aio_request_t *req = io_uring_cqe_get_data(cqe);
            if (req) {
                req->op.result = cqe->res;
                req->op.completed = true;
                if (req->op.callback) {
                    req->op.callback(&req->op, cqe->res, req->op.user_data);
                }
                free_request(req);
                ctx->pending--;
            }
            io_uring_cqe_seen(&ctx->ring, cqe);
            completed++;
        } while (completed < (int)min_completions && io_uring_peek_cqe(&ctx->ring, &cqe) == 0);
    }

    return completed;
}

CXX_C_API uint32_t turbo_aio_pending(turbo_aio_ctx_t *ctx) {
    return ctx ? ctx->pending : 0;
}

CXX_C_API const char *turbo_aio_strerror(int err) {
    switch (err) {
        case TURBO_AIO_OK:        return "Success";
        case TURBO_AIO_EINVAL:    return "Invalid argument";
        case TURBO_AIO_ENOMEM:    return "Out of memory";
        case TURBO_AIO_ENOSYS:    return "Not supported";
        case TURBO_AIO_EBUSY:     return "Queue full";
        case TURBO_AIO_EIO:       return "I/O error";
        case TURBO_AIO_ECANCELED: return "Operation canceled";
        case TURBO_AIO_ETIMEDOUT: return "Timeout";
        default:                  return "Unknown error";
    }
}

#endif /* __linux__ */
