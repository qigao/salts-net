/**
 * @file turbo_aio_uv.c
 * @brief libuv thread pool implementation for async file I/O
 *
 * Used on platforms without kernel-level async I/O: iOS, Android, macOS
 */

#include "turbo_aio.h"

#if defined(__APPLE__) || defined(__ANDROID__) || defined(_WIN32) || defined(TURBO_AIO_USE_UV)

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#ifndef _WIN32
#include <unistd.h>
#endif

// =============================================================================
// Internal Structures
// =============================================================================

typedef struct {
    turbo_aio_op_t op;
    uv_fs_t req;
    uv_buf_t uv_buf;
    char *path;
} aio_request_t;

struct turbo_aio_ctx_s {
    uv_loop_t *loop;
    uint32_t queue_depth;
    uint32_t pending;
    bool owns_loop;
};

// =============================================================================
// Helpers
// =============================================================================

static int flags_to_uv(int flags) {
    int result = 0;
    if (flags & TURBO_AIO_O_RDONLY) result |= UV_FS_O_RDONLY;
    if (flags & TURBO_AIO_O_WRONLY) result |= UV_FS_O_WRONLY;
    if (flags & TURBO_AIO_O_RDWR)   result |= UV_FS_O_RDWR;
    if (flags & TURBO_AIO_O_CREAT)  result |= UV_FS_O_CREAT;
    if (flags & TURBO_AIO_O_TRUNC)  result |= UV_FS_O_TRUNC;
    if (flags & TURBO_AIO_O_APPEND) result |= UV_FS_O_APPEND;
    return result;
}

static void free_request(aio_request_t *req) {
    if (req) {
        if (req->path) free(req->path);
        free(req);
    }
}

static void on_fs_complete(uv_fs_t *req) {
    aio_request_t *aio_req = (aio_request_t *)req->data;
    turbo_aio_ctx_t *ctx = (turbo_aio_ctx_t *)aio_req->op._internal;

    aio_req->op.result = (int)req->result;
    aio_req->op.completed = true;

    if (aio_req->op.callback) {
        aio_req->op.callback(&aio_req->op, (int)req->result, aio_req->op.user_data);
    }

    uv_fs_req_cleanup(req);
    ctx->pending--;
    free_request(aio_req);
}

// =============================================================================
// Context Lifecycle
// =============================================================================

CXX_C_API bool turbo_aio_available(void) {
    return true;
}

CXX_C_API turbo_aio_ctx_t *turbo_aio_create(uint32_t queue_depth) {
    turbo_aio_ctx_t *ctx = (turbo_aio_ctx_t *)calloc(1, sizeof(turbo_aio_ctx_t));
    if (!ctx) return NULL;

    ctx->loop = uv_default_loop();
    ctx->owns_loop = false;
    ctx->queue_depth = queue_depth;
    ctx->pending = 0;
    return ctx;
}

CXX_C_API void turbo_aio_destroy(turbo_aio_ctx_t *ctx) {
    if (!ctx) return;
    free(ctx);
}

// =============================================================================
// Async Operations
// =============================================================================

CXX_C_API int turbo_aio_read(turbo_aio_ctx_t *ctx, int fd, void *buf, size_t len,
                             int64_t offset, turbo_aio_cb cb, void *user_data) {
    if (!ctx || !buf || fd < 0) return TURBO_AIO_EINVAL;

    aio_request_t *req = (aio_request_t *)calloc(1, sizeof(aio_request_t));
    if (!req) return TURBO_AIO_ENOMEM;

    req->op.type = TURBO_AIO_OP_READ;
    req->op.fd = fd;
    req->op.buf = buf;
    req->op.len = len;
    req->op.offset = offset;
    req->op.callback = cb;
    req->op.user_data = user_data;
    req->op._internal = ctx;
    req->req.data = req;
    req->uv_buf = uv_buf_init((char *)buf, (unsigned int)len);

    int err = uv_fs_read(ctx->loop, &req->req, fd, &req->uv_buf, 1, offset, on_fs_complete);
    if (err < 0) {
        free_request(req);
        return TURBO_AIO_EIO;
    }

    ctx->pending++;
    return TURBO_AIO_OK;
}

CXX_C_API int turbo_aio_write(turbo_aio_ctx_t *ctx, int fd, const void *buf, size_t len,
                              int64_t offset, turbo_aio_cb cb, void *user_data) {
    if (!ctx || !buf || fd < 0) return TURBO_AIO_EINVAL;

    aio_request_t *req = (aio_request_t *)calloc(1, sizeof(aio_request_t));
    if (!req) return TURBO_AIO_ENOMEM;

    req->op.type = TURBO_AIO_OP_WRITE;
    req->op.fd = fd;
    req->op.buf = (void *)buf;
    req->op.len = len;
    req->op.offset = offset;
    req->op.callback = cb;
    req->op.user_data = user_data;
    req->op._internal = ctx;
    req->req.data = req;
    req->uv_buf = uv_buf_init((char *)buf, (unsigned int)len);

    int err = uv_fs_write(ctx->loop, &req->req, fd, &req->uv_buf, 1, offset, on_fs_complete);
    if (err < 0) {
        free_request(req);
        return TURBO_AIO_EIO;
    }

    ctx->pending++;
    return TURBO_AIO_OK;
}

CXX_C_API int turbo_aio_fsync(turbo_aio_ctx_t *ctx, int fd, turbo_aio_cb cb, void *user_data) {
    if (!ctx || fd < 0) return TURBO_AIO_EINVAL;

    aio_request_t *req = (aio_request_t *)calloc(1, sizeof(aio_request_t));
    if (!req) return TURBO_AIO_ENOMEM;

    req->op.type = TURBO_AIO_OP_FSYNC;
    req->op.fd = fd;
    req->op.callback = cb;
    req->op.user_data = user_data;
    req->op._internal = ctx;
    req->req.data = req;

    int err = uv_fs_fsync(ctx->loop, &req->req, fd, on_fs_complete);
    if (err < 0) {
        free_request(req);
        return TURBO_AIO_EIO;
    }

    ctx->pending++;
    return TURBO_AIO_OK;
}

CXX_C_API int turbo_aio_open(turbo_aio_ctx_t *ctx, const char *path, int flags, int mode,
                             turbo_aio_cb cb, void *user_data) {
    if (!ctx || !path) return TURBO_AIO_EINVAL;

    aio_request_t *req = (aio_request_t *)calloc(1, sizeof(aio_request_t));
    if (!req) return TURBO_AIO_ENOMEM;

    req->path = strdup(path);
    if (!req->path) {
        free(req);
        return TURBO_AIO_ENOMEM;
    }

    req->op.type = TURBO_AIO_OP_OPEN;
    req->op.callback = cb;
    req->op.user_data = user_data;
    req->op._internal = ctx;
    req->req.data = req;

    int err = uv_fs_open(ctx->loop, &req->req, req->path, flags_to_uv(flags), mode, on_fs_complete);
    if (err < 0) {
        free_request(req);
        return TURBO_AIO_EIO;
    }

    ctx->pending++;
    return TURBO_AIO_OK;
}

CXX_C_API int turbo_aio_close(turbo_aio_ctx_t *ctx, int fd, turbo_aio_cb cb, void *user_data) {
    if (!ctx || fd < 0) return TURBO_AIO_EINVAL;

    aio_request_t *req = (aio_request_t *)calloc(1, sizeof(aio_request_t));
    if (!req) return TURBO_AIO_ENOMEM;

    req->op.type = TURBO_AIO_OP_CLOSE;
    req->op.fd = fd;
    req->op.callback = cb;
    req->op.user_data = user_data;
    req->op._internal = ctx;
    req->req.data = req;

    int err = uv_fs_close(ctx->loop, &req->req, fd, on_fs_complete);
    if (err < 0) {
        free_request(req);
        return TURBO_AIO_EIO;
    }

    ctx->pending++;
    return TURBO_AIO_OK;
}

// =============================================================================
// Completion Handling
// =============================================================================

CXX_C_API int turbo_aio_submit(turbo_aio_ctx_t *ctx) {
    if (!ctx) return TURBO_AIO_EINVAL;
    return ctx->pending;
}

CXX_C_API int turbo_aio_poll(turbo_aio_ctx_t *ctx, uint32_t min_completions, int timeout_ms) {
    if (!ctx) return TURBO_AIO_EINVAL;

    uint32_t initial_pending = ctx->pending;

    if (min_completions == 0) {
        uv_run(ctx->loop, UV_RUN_NOWAIT);
    } else {
        while (ctx->pending > 0 && (initial_pending - ctx->pending) < min_completions) {
            uv_run(ctx->loop, UV_RUN_ONCE);
        }
    }

    return (int)(initial_pending - ctx->pending);
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

#endif /* __APPLE__ || __ANDROID__ || _WIN32 || TURBO_AIO_USE_UV */
