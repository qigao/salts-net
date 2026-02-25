#ifndef S3_BUCKET_CONFIG_H
#define S3_BUCKET_CONFIG_H

#include <platform.h>
#include "s3_error.h"
#include "s3_response.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct s3_client_s s3_client_t;

// ── Tags ──

CXX_C_API s3_tag_set_t s3_get_bucket_tags(s3_client_t* client, const char* bucket, s3_error_t* err);
CXX_C_API s3_error_t   s3_set_bucket_tags(s3_client_t* client, const char* bucket, const s3_tag_set_t* tags);
CXX_C_API s3_error_t   s3_delete_bucket_tags(s3_client_t* client, const char* bucket);

// ── Policy ──

CXX_C_API tstr_t        s3_get_bucket_policy(s3_client_t* client, const char* bucket, s3_error_t* err);
CXX_C_API s3_error_t s3_set_bucket_policy(s3_client_t* client, const char* bucket, const char* policy_json);
CXX_C_API s3_error_t s3_delete_bucket_policy(s3_client_t* client, const char* bucket);

// ── Versioning ──

typedef enum {
    S3_VERSIONING_OFF = 0,
    S3_VERSIONING_ENABLED,
    S3_VERSIONING_SUSPENDED
} s3_versioning_status_t;

CXX_C_API s3_versioning_status_t s3_get_bucket_versioning(s3_client_t* client, const char* bucket, s3_error_t* err);
CXX_C_API s3_error_t s3_set_bucket_versioning(s3_client_t* client, const char* bucket, s3_versioning_status_t status);

// ── Encryption ──

CXX_C_API s3_sse_config_t s3_get_bucket_encryption(s3_client_t* client, const char* bucket, s3_error_t* err);
CXX_C_API s3_error_t      s3_set_bucket_encryption(s3_client_t* client, const char* bucket, const s3_sse_config_t* config);
CXX_C_API s3_error_t      s3_delete_bucket_encryption(s3_client_t* client, const char* bucket);

// ── Object Lock ──

CXX_C_API s3_object_lock_config_t s3_get_object_lock_config(s3_client_t* client, const char* bucket, s3_error_t* err);
CXX_C_API s3_error_t              s3_set_object_lock_config(s3_client_t* client, const char* bucket, const s3_object_lock_config_t* config);
CXX_C_API s3_error_t              s3_delete_object_lock_config(s3_client_t* client, const char* bucket);

#ifdef __cplusplus
}
#endif

#endif // S3_BUCKET_CONFIG_H
