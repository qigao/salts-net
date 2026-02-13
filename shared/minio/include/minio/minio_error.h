#ifndef MINIO_ERROR_H
#define MINIO_ERROR_H

#include <platform.h>
#include <turbo_str.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    code;       // 0 = success, -1 = generic error, or specific S3 error code
    tstr_t message;    // Always owned. Always free with minio_error_free().
} minio_error_t;

#define MINIO_OK ((minio_error_t){0, NULL})

// Create an error with a message (message is duplicated)
CXX_C_API minio_error_t minio_error_make(int code, const char* msg);

// Free an error message
CXX_C_API void minio_error_free(minio_error_t* err);

// Clone an error (duplicates message)
CXX_C_API minio_error_t minio_error_clone(minio_error_t err);

static inline int minio_is_ok(minio_error_t err) {
    return err.code == 0;
}

#ifdef __cplusplus
}
#endif

#endif // MINIO_ERROR_H
