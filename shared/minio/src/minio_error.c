#include "minio/minio_error.h"
#include <turbo_str.h>
#include <stdlib.h>

minio_error_t minio_error_make(int code, const char* msg) {
    minio_error_t err;
    err.code = code;
    err.message = msg ? tstr_dup(msg) : NULL;
    return err;
}

void minio_error_free(minio_error_t* err) {
    if (!err) return;
    if (err->message) {
        tstr_free(err->message);
        err->message = NULL;
    }
}

minio_error_t minio_error_clone(minio_error_t err) {
    minio_error_t res = err;
    if (err.message) res.message = tstr_dup(err.message);
    return res;
}
