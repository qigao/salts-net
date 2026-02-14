#ifndef S3_NOTIFICATION_H
#define S3_NOTIFICATION_H

#include <turbo_str.h>
#include <platform.h>
#include "s3_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct s3_client_s s3_client_t;

typedef struct {
    tstr_t id;
    tstr_t arn;
    tstr_t* events;
    int event_count;
    tstr_t prefix_filter;
    tstr_t suffix_filter;
} s3_notification_rule_t;

typedef struct {
    s3_notification_rule_t* queue_configs;
    int queue_count;
    s3_notification_rule_t* topic_configs;
    int topic_count;
    s3_notification_rule_t* cloud_func_configs;
    int cloud_func_count;
} s3_notification_config_t;

CXX_C_API s3_notification_config_t s3_get_bucket_notification(s3_client_t* client, const char* bucket, s3_error_t* err);
CXX_C_API s3_error_t               s3_set_bucket_notification(s3_client_t* client, const char* bucket, const s3_notification_config_t* config);
CXX_C_API s3_error_t               s3_delete_bucket_notification(s3_client_t* client, const char* bucket);
CXX_C_API void                        s3_notification_config_free(s3_notification_config_t* config);

typedef int (*s3_notification_callback_t)(const char* json_event, void* userdata);

CXX_C_API s3_error_t s3_listen_bucket_notification(
    s3_client_t* client, const char* bucket,
    const char* prefix, const char* suffix,
    const char** events, int event_count,
    s3_notification_callback_t callback, void* userdata);

#ifdef __cplusplus
}
#endif

#endif // S3_NOTIFICATION_H
