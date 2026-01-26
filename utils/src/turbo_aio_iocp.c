/**
 * @file turbo_aio_iocp.c
 * @brief Windows IOCP implementation for async file I/O
 */

#include "turbo_aio.h"

#ifdef _WIN32

#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// =============================================================================
// Internal Structures
// =============================================================================

typedef struct aio_request_s {
    OVERLAPPED overlapped;
    turbo_aio_op_t op;
    char *path;
    HANDLE file_handle;
} aio_request_t;

struct turbo_aio_ctx_s {
    HANDLE iocp;
    uint32_t queue_depth;
    uint32_t pending;
    bool initialized;
};

// =============================================================================
// Helpers
// =============================================================================

static DWORD flags_to_win32_access(int flags) {
    DWORD access = 0;
    if (flags & TURBO_AIO_O_RDONLY) access |= GENERIC_READ;
    if (flags & TURBO_AIO_O_WRONLY) access |= GENERIC_WRITE;
    if (flags & TURBO_AIO_O_RDWR)   access |= GENERIC_READ | GENERIC_WRITE;
    return access;
}

static DWORD flags_to_win32_creation(int flags) {
    if (flags & TURBO_AIO_O_CREAT) {
        if (flags & TURBO_AIO_O_TRUNC) return CREATE_ALWAYS;
        return OPEN_ALWAYS;
    }
    if (flags & TURBO_AIO_O_TRUNC) return TRUNCATE_EXISTING;
    return OPEN_EXISTING;
}

static DWORD flags_to_win32_flags(int flags) {
    DWORD result = FILE_FLAG_OVERLAPPED;
    if (flags & TURBO_AIO_O_DIRECT) result |= FILE_FLAG_NO_BUFFERING;
    return result;
}

static aio_request_t *alloc_request(turbo_aio_op_type_t type, int fd,
                                    void *buf, size_t len, int64_t offset,
                                    turbo_aio_cb cb, void *user_data) {
    aio_request_t *req = (aio_request_t *)calloc(1, sizeof(aio_request_t));
    if (!req) return NULL;
    
    memset(&req->overlapped, 0, sizeof(OVERLAPPED));
    if (offset >= 0) {
        req->overlapped.Offset = (DWORD)(offset & 0xFFFFFFFF);
        req->overlapped.OffsetHigh = (DWORD)(offset >> 32);
    }
    
    req->op.type = type;
    req->op.fd = fd;
    req->op.buf = buf;
    req->op.len = len;
    req->op.offset = offset;
    req->op.callback = cb;
    req->op.user_data = user_data;
    req->op.completed = false;
    req->file_handle = INVALID_HANDLE_VALUE;
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
    return true;  /* IOCP always available on Windows */
}

CXX_C_API turbo_aio_ctx_t *turbo_aio_create(uint32_t queue_depth) {
    turbo_aio_ctx_t *ctx = (turbo_aio_ctx_t *)calloc(1, sizeof(turbo_aio_ctx_t));
    if (!ctx) return NULL;

    ctx->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (ctx->iocp == NULL) {
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
    if (ctx->iocp) {
        CloseHandle(ctx->iocp);
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

    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        free_request(req);
        return TURBO_AIO_EINVAL;
    }

    /* Associate with IOCP */
    if (CreateIoCompletionPort(h, ctx->iocp, (ULONG_PTR)req, 0) == NULL) {
        free_request(req);
        return TURBO_AIO_EIO;
    }

    DWORD bytes_read;
    BOOL success = ReadFile(h, buf, (DWORD)len, &bytes_read, &req->overlapped);
    
    if (!success && GetLastError() != ERROR_IO_PENDING) {
        free_request(req);
        return TURBO_AIO_EIO;
    }

    ctx->pending++;
    return TURBO_AIO_OK;
}

CXX_C_API int turbo_aio_write(turbo_aio_ctx_t *ctx, int fd, const void *buf, size_t len,
                              int64_t offset, turbo_aio_cb cb, void *user_data) {
    if (!ctx || !buf || fd < 0) return TURBO_AIO_EINVAL;

    aio_request_t *req = alloc_request(TURBO_AIO_OP_WRITE, fd, (void *)buf, len, offset, cb, user_data);
    if (!req) return TURBO_AIO_ENOMEM;

    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        free_request(req);
        return TURBO_AIO_EINVAL;
    }

    if (CreateIoCompletionPort(h, ctx->iocp, (ULONG_PTR)req, 0) == NULL) {
        free_request(req);
        return TURBO_AIO_EIO;
    }

    DWORD bytes_written;
    BOOL success = WriteFile(h, buf, (DWORD)len, &bytes_written, &req->overlapped);
    
    if (!success && GetLastError() != ERROR_IO_PENDING) {
        free_request(req);
        return TURBO_AIO_EIO;
    }

    ctx->pending++;
    return TURBO_AIO_OK;
}

CXX_C_API int turbo_aio_fsync(turbo_aio_ctx_t *ctx, int fd, turbo_aio_cb cb, void *user_data) {
    if (!ctx || fd < 0) return TURBO_AIO_EINVAL;

    /* Windows FlushFileBuffers is synchronous, simulate async */
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return TURBO_AIO_EINVAL;

    BOOL success = FlushFileBuffers(h);
    int result = success ? 0 : TURBO_AIO_EIO;
    
    if (cb) {
        turbo_aio_op_t op = {0};
        op.type = TURBO_AIO_OP_FSYNC;
        op.fd = fd;
        op.result = result;
        op.completed = true;
        cb(&op, result, user_data);
    }
    
    return TURBO_AIO_OK;
}

CXX_C_API int turbo_aio_open(turbo_aio_ctx_t *ctx, const char *path, int flags, int mode,
                             turbo_aio_cb cb, void *user_data) {
    if (!ctx || !path) return TURBO_AIO_EINVAL;
    (void)mode;  /* Windows ignores POSIX mode */

    DWORD access = flags_to_win32_access(flags);
    DWORD creation = flags_to_win32_creation(flags);
    DWORD attrs = flags_to_win32_flags(flags);

    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, creation, attrs, NULL);
    
    int result;
    if (h == INVALID_HANDLE_VALUE) {
        result = TURBO_AIO_EIO;
    } else {
        result = _open_osfhandle((intptr_t)h, 0);
        if (result == -1) {
            CloseHandle(h);
            result = TURBO_AIO_EIO;
        }
    }

    if (cb) {
        turbo_aio_op_t op = {0};
        op.type = TURBO_AIO_OP_OPEN;
        op.result = result;
        op.completed = true;
        cb(&op, result, user_data);
    }

    return TURBO_AIO_OK;
}

CXX_C_API int turbo_aio_close(turbo_aio_ctx_t *ctx, int fd, turbo_aio_cb cb, void *user_data) {
    if (!ctx || fd < 0) return TURBO_AIO_EINVAL;

    int result = _close(fd) == 0 ? 0 : TURBO_AIO_EIO;

    if (cb) {
        turbo_aio_op_t op = {0};
        op.type = TURBO_AIO_OP_CLOSE;
        op.fd = fd;
        op.result = result;
        op.completed = true;
        cb(&op, result, user_data);
    }

    return TURBO_AIO_OK;
}

// =============================================================================
// Completion Handling
// =============================================================================

CXX_C_API int turbo_aio_submit(turbo_aio_ctx_t *ctx) {
    if (!ctx) return TURBO_AIO_EINVAL;
    return ctx->pending;  /* IOCP submits immediately */
}

CXX_C_API int turbo_aio_poll(turbo_aio_ctx_t *ctx, uint32_t min_completions, int timeout_ms) {
    if (!ctx) return TURBO_AIO_EINVAL;

    DWORD timeout = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    int completed = 0;

    while ((uint32_t)completed < min_completions || (min_completions == 0 && completed == 0)) {
        DWORD bytes;
        ULONG_PTR key;
        LPOVERLAPPED overlapped;

        BOOL success = GetQueuedCompletionStatus(ctx->iocp, &bytes, &key, &overlapped,
                                                  min_completions == 0 ? 0 : timeout);
        
        if (!success && overlapped == NULL) {
            if (GetLastError() == WAIT_TIMEOUT) break;
            if (min_completions == 0) break;
            return TURBO_AIO_EIO;
        }

        if (overlapped) {
            aio_request_t *req = CONTAINING_RECORD(overlapped, aio_request_t, overlapped);
            req->op.result = success ? (int)bytes : TURBO_AIO_EIO;
            req->op.completed = true;
            
            if (req->op.callback) {
                req->op.callback(&req->op, req->op.result, req->op.user_data);
            }
            
            free_request(req);
            ctx->pending--;
            completed++;
        }

        if (min_completions == 0) break;
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

#endif /* _WIN32 */
