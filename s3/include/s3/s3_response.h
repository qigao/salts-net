#ifndef S3_RESPONSE_H
#define S3_RESPONSE_H

#include <platform.h>
#include "s3_types.h"
#include "s3_error.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API s3_error_t s3_parse_error_xml(const char* xml_data);

typedef struct {
    S3BucketVec* buckets;
    s3_error_t error;
} s3_list_buckets_parser_res_t;

CXX_C_API s3_list_buckets_parser_res_t s3_parse_list_buckets_xml(const char* xml_data);

typedef struct {
    S3ItemVec* items;
    int is_truncated;
    tstr_t next_continuation_token;
    s3_error_t error;
} s3_list_objects_parser_res_t;

CXX_C_API s3_list_objects_parser_res_t s3_parse_list_objects_xml(const char* xml_data);
CXX_C_API void s3_list_objects_parser_res_free(s3_list_objects_parser_res_t* res);

// Multipart XML parsers
CXX_C_API tstr_t s3_parse_create_multipart_xml(const char* xml_data, s3_error_t* err);
CXX_C_API tstr_t s3_parse_complete_multipart_etag_xml(const char* xml_data, tstr_t* location, s3_error_t* err);
CXX_C_API tstr_t s3_parse_upload_part_copy_etag_xml(const char* xml_data, s3_error_t* err);

// Batch delete XML parser
typedef struct {
    tstr_t key;
    tstr_t version_id;
    tstr_t error_code;
    tstr_t error_message;
} s3_delete_error_t;

typedef struct {
    s3_delete_error_t* errors;
    int error_count;
    s3_error_t error;
} s3_delete_objects_parser_res_t;

CXX_C_API s3_delete_objects_parser_res_t s3_parse_delete_objects_xml(const char* xml_data);
CXX_C_API void s3_delete_objects_parser_res_free(s3_delete_objects_parser_res_t* res);

// Copy object XML parser
typedef struct {
    tstr_t etag;
    tstr_t last_modified_str;
    s3_error_t error;
} s3_copy_object_parser_res_t;

CXX_C_API s3_copy_object_parser_res_t s3_parse_copy_object_xml(const char* xml_data);

// Tags XML parser
typedef struct { tstr_t key; tstr_t value; } s3_tag_t;
typedef struct { s3_tag_t* tags; int count; } s3_tag_set_t;

CXX_C_API s3_tag_set_t s3_parse_tagging_xml(const char* xml_data, s3_error_t* err);
CXX_C_API void s3_tag_set_free(s3_tag_set_t* tags);

// Versioning XML parser
CXX_C_API tstr_t s3_parse_versioning_xml(const char* xml_data, s3_error_t* err);

// Encryption XML parser
typedef struct {
    int is_kms;
    tstr_t kms_master_key_id;
} s3_sse_config_t;

CXX_C_API s3_sse_config_t s3_parse_encryption_xml(const char* xml_data, s3_error_t* err);
CXX_C_API void s3_sse_config_free(s3_sse_config_t* config);

// Object lock XML parser
typedef enum { S3_RETENTION_GOVERNANCE = 0, S3_RETENTION_COMPLIANCE = 1 } s3_retention_mode_t;

typedef struct {
    int enabled;
    s3_retention_mode_t mode;
    int days;
    int years;
} s3_object_lock_config_t;

CXX_C_API s3_object_lock_config_t s3_parse_object_lock_xml(const char* xml_data, s3_error_t* err);

// Object retention XML parser
typedef struct {
    s3_retention_mode_t mode;
    time_t retain_until_date;
} s3_object_retention_t;

CXX_C_API s3_object_retention_t s3_parse_object_retention_xml(const char* xml_data, s3_error_t* err);

// Legal hold XML parser
CXX_C_API int s3_parse_legal_hold_xml(const char* xml_data, s3_error_t* err);

#ifdef __cplusplus
}
#endif

#endif // S3_RESPONSE_H
