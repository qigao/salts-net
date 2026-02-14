#ifndef S3_ERROR_H
#define S3_ERROR_H

#include <platform.h>
#include <turbo_str.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    code;       // 0 = success, -1 = generic error, or specific S3 error code
    tstr_t message;    // Always owned. Always free with s3_error_free().
} s3_error_t;

#define S3_OK ((s3_error_t){0, NULL})

// Create an error with a message (message is duplicated)
CXX_C_API s3_error_t s3_error_make(int code, const char* msg);

// Free an error message
CXX_C_API void s3_error_free(s3_error_t* err);

// Clone an error (duplicates message)
CXX_C_API s3_error_t s3_error_clone(s3_error_t err);

static inline int s3_is_ok(s3_error_t err) {
    return err.code == 0;
}

#ifdef __cplusplus
}
#endif

#endif // S3_ERROR_H
