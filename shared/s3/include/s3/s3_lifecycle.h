#ifndef S3_LIFECYCLE_H
#define S3_LIFECYCLE_H

#include <turbo_str.h>
#include <platform.h>
#include <time.h>
#include "s3_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct s3_client_s s3_client_t;

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
} s3_lifecycle_rule_t;

typedef struct {
    s3_lifecycle_rule_t* rules;
    int count;
} s3_lifecycle_config_t;

CXX_C_API s3_lifecycle_config_t s3_get_bucket_lifecycle(s3_client_t* client, const char* bucket, s3_error_t* err);
CXX_C_API s3_error_t            s3_set_bucket_lifecycle(s3_client_t* client, const char* bucket, const s3_lifecycle_config_t* config);
CXX_C_API s3_error_t            s3_delete_bucket_lifecycle(s3_client_t* client, const char* bucket);
CXX_C_API tstr_t                   s3_lifecycle_config_to_xml(const s3_lifecycle_config_t* config);
CXX_C_API s3_lifecycle_config_t s3_lifecycle_config_from_xml(const char* xml, s3_error_t* err);
CXX_C_API void                     s3_lifecycle_config_free(s3_lifecycle_config_t* config);

#ifdef __cplusplus
}
#endif

#endif // S3_LIFECYCLE_H
