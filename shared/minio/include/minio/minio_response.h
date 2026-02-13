#ifndef MINIO_RESPONSE_H
#define MINIO_RESPONSE_H

#include <platform.h>
#include "minio_types.h"
#include "minio_error.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API minio_error_t minio_parse_error_xml(const char* xml_data);

typedef struct {
    MinioBucketVec buckets;
    minio_error_t error;
} minio_list_buckets_parser_res_t;

CXX_C_API minio_list_buckets_parser_res_t minio_parse_list_buckets_xml(const char* xml_data);

typedef struct {
    MinioItemVec items;
    int is_truncated;
    tstr_t next_continuation_token;
    minio_error_t error;
} minio_list_objects_parser_res_t;

CXX_C_API minio_list_objects_parser_res_t minio_parse_list_objects_xml(const char* xml_data);
CXX_C_API void minio_list_objects_parser_res_free(minio_list_objects_parser_res_t* res);

// Multipart XML parsers
CXX_C_API tstr_t minio_parse_create_multipart_xml(const char* xml_data, minio_error_t* err);
CXX_C_API tstr_t minio_parse_complete_multipart_etag_xml(const char* xml_data, tstr_t* location, minio_error_t* err);
CXX_C_API tstr_t minio_parse_upload_part_copy_etag_xml(const char* xml_data, minio_error_t* err);

// Batch delete XML parser
typedef struct {
    tstr_t key;
    tstr_t version_id;
    tstr_t error_code;
    tstr_t error_message;
} minio_delete_error_t;

typedef struct {
    minio_delete_error_t* errors;
    int error_count;
    minio_error_t error;
} minio_delete_objects_parser_res_t;

CXX_C_API minio_delete_objects_parser_res_t minio_parse_delete_objects_xml(const char* xml_data);
CXX_C_API void minio_delete_objects_parser_res_free(minio_delete_objects_parser_res_t* res);

// Copy object XML parser
typedef struct {
    tstr_t etag;
    tstr_t last_modified_str;
    minio_error_t error;
} minio_copy_object_parser_res_t;

CXX_C_API minio_copy_object_parser_res_t minio_parse_copy_object_xml(const char* xml_data);

// Tags XML parser
typedef struct { tstr_t key; tstr_t value; } minio_tag_t;
typedef struct { minio_tag_t* tags; int count; } minio_tag_set_t;

CXX_C_API minio_tag_set_t minio_parse_tagging_xml(const char* xml_data, minio_error_t* err);
CXX_C_API void minio_tag_set_free(minio_tag_set_t* tags);

// Versioning XML parser
CXX_C_API tstr_t minio_parse_versioning_xml(const char* xml_data, minio_error_t* err);

// Encryption XML parser
typedef struct {
    int is_kms;
    tstr_t kms_master_key_id;
} minio_sse_config_t;

CXX_C_API minio_sse_config_t minio_parse_encryption_xml(const char* xml_data, minio_error_t* err);
CXX_C_API void minio_sse_config_free(minio_sse_config_t* config);

// Object lock XML parser
typedef enum { MINIO_RETENTION_GOVERNANCE = 0, MINIO_RETENTION_COMPLIANCE = 1 } minio_retention_mode_t;

typedef struct {
    int enabled;
    minio_retention_mode_t mode;
    int days;
    int years;
} minio_object_lock_config_t;

CXX_C_API minio_object_lock_config_t minio_parse_object_lock_xml(const char* xml_data, minio_error_t* err);

// Object retention XML parser
typedef struct {
    minio_retention_mode_t mode;
    time_t retain_until_date;
} minio_object_retention_t;

CXX_C_API minio_object_retention_t minio_parse_object_retention_xml(const char* xml_data, minio_error_t* err);

// Legal hold XML parser
CXX_C_API int minio_parse_legal_hold_xml(const char* xml_data, minio_error_t* err);

#ifdef __cplusplus
}
#endif

#endif // MINIO_RESPONSE_H
