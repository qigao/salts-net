#ifndef MINIO_LIFECYCLE_H
#define MINIO_LIFECYCLE_H

#include <turbo_str.h>
#include <platform.h>
#include <time.h>
#include "minio_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct minio_client_s minio_client_t;

typedef struct {
    tstr_t id;
    tstr_t prefix;
    int enabled;
    int expiration_days;
    time_t expiration_date;
    int transition_days;
    tstr_t transition_storage_class;
    int noncurrent_expiration_days;
    int noncurrent_transition_days;
    tstr_t noncurrent_transition_storage_class;
    int abort_incomplete_days;
} minio_lifecycle_rule_t;

typedef struct {
    minio_lifecycle_rule_t* rules;
    int count;
} minio_lifecycle_config_t;

CXX_C_API minio_lifecycle_config_t minio_get_bucket_lifecycle(minio_client_t* client, const char* bucket, minio_error_t* err);
CXX_C_API minio_error_t            minio_set_bucket_lifecycle(minio_client_t* client, const char* bucket, const minio_lifecycle_config_t* config);
CXX_C_API minio_error_t            minio_delete_bucket_lifecycle(minio_client_t* client, const char* bucket);
CXX_C_API tstr_t                   minio_lifecycle_config_to_xml(const minio_lifecycle_config_t* config);
CXX_C_API minio_lifecycle_config_t minio_lifecycle_config_from_xml(const char* xml, minio_error_t* err);
CXX_C_API void                     minio_lifecycle_config_free(minio_lifecycle_config_t* config);

#ifdef __cplusplus
}
#endif

#endif // MINIO_LIFECYCLE_H
