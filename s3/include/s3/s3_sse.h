#ifndef S3_SSE_H
#define S3_SSE_H

#include <turbo_str.h>
#include <platform.h>
#include "s3_multimap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    S3_SSE_NONE = 0,
    S3_SSE_S3,
    S3_SSE_KMS,
    S3_SSE_C
} s3_sse_type_t;

typedef struct {
    s3_sse_type_t type;
    tstr_t kms_key_id;        // SSE-KMS only
    tstr_t kms_context;       // SSE-KMS only
    tstr_t customer_key_b64;  // SSE-C only (base64)
    tstr_t customer_key_md5;  // SSE-C only (base64 MD5)
} s3_sse_t;

CXX_C_API s3_sse_t s3_sse_s3(void);
CXX_C_API s3_sse_t s3_sse_kms(const char* key_id, const char* context);
CXX_C_API s3_sse_t s3_sse_customer_key(const char* key_b64);
CXX_C_API void s3_sse_free(s3_sse_t* sse);
CXX_C_API void s3_sse_apply_headers(const s3_sse_t* sse, S3Headers* headers);
CXX_C_API void s3_sse_apply_copy_headers(const s3_sse_t* sse, S3Headers* headers);

#ifdef __cplusplus
}
#endif

#endif // S3_SSE_H
