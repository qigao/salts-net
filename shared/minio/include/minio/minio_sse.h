#ifndef MINIO_SSE_H
#define MINIO_SSE_H

#include <turbo_str.h>
#include <platform.h>
#include "minio_multimap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MINIO_SSE_NONE = 0,
    MINIO_SSE_S3,
    MINIO_SSE_KMS,
    MINIO_SSE_C
} minio_sse_type_t;

typedef struct {
    minio_sse_type_t type;
    tstr_t kms_key_id;        // SSE-KMS only
    tstr_t kms_context;       // SSE-KMS only
    tstr_t customer_key_b64;  // SSE-C only (base64)
    tstr_t customer_key_md5;  // SSE-C only (base64 MD5)
} minio_sse_t;

CXX_C_API minio_sse_t minio_sse_s3(void);
CXX_C_API minio_sse_t minio_sse_kms(const char* key_id, const char* context);
CXX_C_API minio_sse_t minio_sse_customer_key(const char* key_b64);
CXX_C_API void minio_sse_free(minio_sse_t* sse);
CXX_C_API void minio_sse_apply_headers(const minio_sse_t* sse, MinioHeaders* headers);
CXX_C_API void minio_sse_apply_copy_headers(const minio_sse_t* sse, MinioHeaders* headers);

#ifdef __cplusplus
}
#endif

#endif // MINIO_SSE_H
