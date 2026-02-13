#ifndef MINIO_REPLICATION_H
#define MINIO_REPLICATION_H

#include <turbo_str.h>
#include <platform.h>
#include "minio_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct minio_client_s minio_client_t;

typedef struct {
    tstr_t id;
    tstr_t prefix;
    int enabled;
    tstr_t destination_bucket_arn;
    tstr_t destination_storage_class;
    int replicate_deletes;
    int replicate_delete_markers;
} minio_replication_rule_t;

typedef struct {
    tstr_t role;
    minio_replication_rule_t* rules;
    int count;
} minio_replication_config_t;

CXX_C_API minio_replication_config_t minio_get_bucket_replication(minio_client_t* client, const char* bucket, minio_error_t* err);
CXX_C_API minio_error_t              minio_set_bucket_replication(minio_client_t* client, const char* bucket, const minio_replication_config_t* config);
CXX_C_API minio_error_t              minio_delete_bucket_replication(minio_client_t* client, const char* bucket);
CXX_C_API void                       minio_replication_config_free(minio_replication_config_t* config);

#ifdef __cplusplus
}
#endif

#endif // MINIO_REPLICATION_H
