#ifndef MINIO_BUCKET_CONFIG_H
#define MINIO_BUCKET_CONFIG_H

#include <platform.h>
#include "minio_error.h"
#include "minio_response.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct minio_client_s minio_client_t;

// ── Tags ──

CXX_C_API minio_tag_set_t minio_get_bucket_tags(minio_client_t* client, const char* bucket, minio_error_t* err);
CXX_C_API minio_error_t   minio_set_bucket_tags(minio_client_t* client, const char* bucket, const minio_tag_set_t* tags);
CXX_C_API minio_error_t   minio_delete_bucket_tags(minio_client_t* client, const char* bucket);

// ── Policy ──

CXX_C_API tstr_t        minio_get_bucket_policy(minio_client_t* client, const char* bucket, minio_error_t* err);
CXX_C_API minio_error_t minio_set_bucket_policy(minio_client_t* client, const char* bucket, const char* policy_json);
CXX_C_API minio_error_t minio_delete_bucket_policy(minio_client_t* client, const char* bucket);

// ── Versioning ──

typedef enum {
    MINIO_VERSIONING_OFF = 0,
    MINIO_VERSIONING_ENABLED,
    MINIO_VERSIONING_SUSPENDED
} minio_versioning_status_t;

CXX_C_API minio_versioning_status_t minio_get_bucket_versioning(minio_client_t* client, const char* bucket, minio_error_t* err);
CXX_C_API minio_error_t minio_set_bucket_versioning(minio_client_t* client, const char* bucket, minio_versioning_status_t status);

// ── Encryption ──

CXX_C_API minio_sse_config_t minio_get_bucket_encryption(minio_client_t* client, const char* bucket, minio_error_t* err);
CXX_C_API minio_error_t      minio_set_bucket_encryption(minio_client_t* client, const char* bucket, const minio_sse_config_t* config);
CXX_C_API minio_error_t      minio_delete_bucket_encryption(minio_client_t* client, const char* bucket);

// ── Object Lock ──

CXX_C_API minio_object_lock_config_t minio_get_object_lock_config(minio_client_t* client, const char* bucket, minio_error_t* err);
CXX_C_API minio_error_t              minio_set_object_lock_config(minio_client_t* client, const char* bucket, const minio_object_lock_config_t* config);
CXX_C_API minio_error_t              minio_delete_object_lock_config(minio_client_t* client, const char* bucket);

#ifdef __cplusplus
}
#endif

#endif // MINIO_BUCKET_CONFIG_H
