#ifndef S3_REPLICATION_H
#define S3_REPLICATION_H

#include <turbo_str.h>
#include <platform.h>
#include "s3_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct s3_client_s s3_client_t;

typedef struct {
    tstr_t id;
    tstr_t prefix;
    int enabled;
    tstr_t destination_bucket_arn;
    tstr_t destination_storage_class;
    int replicate_deletes;
    int replicate_delete_markers;
} s3_replication_rule_t;

typedef struct {
    tstr_t role;
    s3_replication_rule_t* rules;
    int count;
} s3_replication_config_t;

CXX_C_API s3_replication_config_t s3_get_bucket_replication(s3_client_t* client, const char* bucket, s3_error_t* err);
CXX_C_API s3_error_t              s3_set_bucket_replication(s3_client_t* client, const char* bucket, const s3_replication_config_t* config);
CXX_C_API s3_error_t              s3_delete_bucket_replication(s3_client_t* client, const char* bucket);
CXX_C_API void                       s3_replication_config_free(s3_replication_config_t* config);

#ifdef __cplusplus
}
#endif

#endif // S3_REPLICATION_H
