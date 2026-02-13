#ifndef MINIO_NOTIFICATION_H
#define MINIO_NOTIFICATION_H

#include <turbo_str.h>
#include <platform.h>
#include "minio_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct minio_client_s minio_client_t;

typedef struct {
    tstr_t id;
    tstr_t arn;
    tstr_t* events;
    int event_count;
    tstr_t prefix_filter;
    tstr_t suffix_filter;
} minio_notification_rule_t;

typedef struct {
    minio_notification_rule_t* queue_configs;
    int queue_count;
    minio_notification_rule_t* topic_configs;
    int topic_count;
    minio_notification_rule_t* cloud_func_configs;
    int cloud_func_count;
} minio_notification_config_t;

CXX_C_API minio_notification_config_t minio_get_bucket_notification(minio_client_t* client, const char* bucket, minio_error_t* err);
CXX_C_API minio_error_t               minio_set_bucket_notification(minio_client_t* client, const char* bucket, const minio_notification_config_t* config);
CXX_C_API minio_error_t               minio_delete_bucket_notification(minio_client_t* client, const char* bucket);
CXX_C_API void                        minio_notification_config_free(minio_notification_config_t* config);

typedef int (*minio_notification_callback_t)(const char* json_event, void* userdata);

CXX_C_API minio_error_t minio_listen_bucket_notification(
    minio_client_t* client, const char* bucket,
    const char* prefix, const char* suffix,
    const char** events, int event_count,
    minio_notification_callback_t callback, void* userdata);

#ifdef __cplusplus
}
#endif

#endif // MINIO_NOTIFICATION_H
