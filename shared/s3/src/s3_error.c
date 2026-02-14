#include "s3/s3_error.h"
#include <turbo_str.h>
#include <stdlib.h>

s3_error_t s3_error_make(int code, const char* msg) {
    s3_error_t err;
    err.code = code;
    err.message = msg ? tstr_dup(msg) : NULL;
    return err;
}

void s3_error_free(s3_error_t* err) {
    if (!err) return;
    if (err->message) {
        tstr_free(err->message);
        err->message = NULL;
    }
}

s3_error_t s3_error_clone(s3_error_t err) {
    s3_error_t res = err;
    if (err.message) res.message = tstr_dup(err.message);
    return res;
}
