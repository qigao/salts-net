#ifndef S3_CLIENT_H
#define S3_CLIENT_H

#include <turbo_str.h>
#include <platform.h>
#include <netcore/turbo_coro_context.h>
#include "s3_types.h"
#include "s3_credentials.h"
#include "s3_error.h"
#include "s3_response.h"
#include "s3_sse.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct s3_client_s s3_client_t;

typedef struct {
    tstr_t host;
    int    port;
    int    is_https;
    tstr_t region;
    int    virtual_style; // 1 for virtual-hosted, 0 for path-style
} s3_base_url_t;

CXX_C_API void s3_base_url_free(s3_base_url_t* url);

// Client lifecycle
CXX_C_API s3_client_t* s3_client_create(turbo_coro_context_t *ctx,
                                   const s3_base_url_t* base_url,
                                   s3_credential_provider_t* provider);
CXX_C_API void            s3_client_destroy(s3_client_t* client);
CXX_C_API void            s3_client_set_part_size(s3_client_t* client, size_t part_size);

// ── Bucket operations ──

CXX_C_API s3_error_t s3_make_bucket(s3_client_t* client, const char* bucket, const char* region);
CXX_C_API s3_error_t s3_remove_bucket(s3_client_t* client, const char* bucket);
CXX_C_API int           s3_bucket_exists(s3_client_t* client, const char* bucket, s3_error_t* err);

typedef struct {
    S3BucketVec  buckets;
    s3_error_t   error;
} s3_list_buckets_response_t;

CXX_C_API s3_list_buckets_response_t s3_list_buckets(s3_client_t* client);
CXX_C_API void s3_list_buckets_free(s3_list_buckets_response_t* resp);

// ── Object CRUD ──

CXX_C_API s3_error_t s3_upload_object(s3_client_t* client,
                                 const char* bucket, const char* object,
                                 const char* filename);

CXX_C_API s3_error_t s3_put_object(s3_client_t* client,
                              const char* bucket, const char* object,
                              const char* data, size_t len,
                              const char* content_type);

CXX_C_API s3_error_t s3_download_object(s3_client_t* client,
                                   const char* bucket, const char* object,
                                   const char* filename);

typedef int (*s3_data_callback_t)(const char* data, size_t len, void* userdata);

CXX_C_API s3_error_t s3_get_object(s3_client_t* client,
                              const char* bucket, const char* object,
                              s3_data_callback_t callback, void* userdata);

typedef struct {
    tstr_t  etag;
    size_t  size;
    time_t  last_modified;
    tstr_t  content_type;
    s3_error_t error;
} s3_stat_object_response_t;

CXX_C_API void s3_stat_object_response_free(s3_stat_object_response_t* resp);

CXX_C_API s3_stat_object_response_t s3_stat_object(s3_client_t* client,
                                              const char* bucket,
                                              const char* object);

CXX_C_API s3_error_t s3_remove_object(s3_client_t* client,
                                 const char* bucket, const char* object);

// List objects (iterator-based)
typedef struct s3_list_objects_iter_s s3_list_objects_iter_t;

CXX_C_API s3_list_objects_iter_t* s3_list_objects(s3_client_t* client,
                                            const char* bucket,
                                            const char* prefix,
                                            int recursive);
CXX_C_API int           s3_list_objects_next(s3_list_objects_iter_t* iter, s3_item_t* item);
CXX_C_API s3_error_t s3_list_objects_error(s3_list_objects_iter_t* iter);
CXX_C_API void          s3_list_objects_free(s3_list_objects_iter_t* iter);

// ── Multipart Upload ──

typedef struct {
    tstr_t upload_id;
    s3_error_t error;
} s3_create_multipart_response_t;

typedef struct {
    tstr_t etag;
    s3_error_t error;
} s3_upload_part_response_t;

typedef struct {
    tstr_t etag;
    tstr_t location;
    s3_error_t error;
} s3_complete_multipart_response_t;

CXX_C_API s3_create_multipart_response_t s3_create_multipart_upload(
    s3_client_t* client, const char* bucket, const char* object,
    const char* content_type, const s3_sse_t* sse);

CXX_C_API s3_upload_part_response_t s3_upload_part(
    s3_client_t* client, const char* bucket, const char* object,
    const char* upload_id, int part_number,
    const char* data, size_t len);

CXX_C_API s3_upload_part_response_t s3_upload_part_copy(
    s3_client_t* client, const char* bucket, const char* object,
    const char* upload_id, int part_number,
    const char* src_bucket, const char* src_object,
    size_t offset, size_t length);

CXX_C_API s3_complete_multipart_response_t s3_complete_multipart_upload(
    s3_client_t* client, const char* bucket, const char* object,
    const char* upload_id, const char** etags, int part_count);

CXX_C_API s3_error_t s3_abort_multipart_upload(
    s3_client_t* client, const char* bucket, const char* object,
    const char* upload_id);

CXX_C_API void s3_create_multipart_response_free(s3_create_multipart_response_t* resp);
CXX_C_API void s3_upload_part_response_free(s3_upload_part_response_t* resp);
CXX_C_API void s3_complete_multipart_response_free(s3_complete_multipart_response_t* resp);

// ── Copy / Compose ──

typedef struct {
    tstr_t etag;
    time_t last_modified;
    s3_error_t error;
} s3_copy_object_response_t;

CXX_C_API s3_copy_object_response_t s3_copy_object(
    s3_client_t* client,
    const char* src_bucket, const char* src_object,
    const char* dst_bucket, const char* dst_object);

typedef struct {
    const char* bucket;
    const char* object;
    size_t offset;
    size_t length;
} s3_compose_source_t;

CXX_C_API s3_copy_object_response_t s3_compose_object(
    s3_client_t* client,
    const char* bucket, const char* object,
    const s3_compose_source_t* sources, int source_count);

// ── Batch Delete ──

typedef struct {
    s3_delete_error_t* errors;
    int error_count;
    s3_error_t error;
} s3_remove_objects_response_t;

CXX_C_API s3_remove_objects_response_t s3_remove_objects(
    s3_client_t* client, const char* bucket,
    const char** keys, int key_count);

CXX_C_API void s3_remove_objects_response_free(s3_remove_objects_response_t* resp);

// ── Presigned URLs ──

CXX_C_API tstr_t s3_get_presigned_object_url(
    s3_client_t* client, const char* method,
    const char* bucket, const char* object,
    int expires_secs);

typedef struct {
    tstr_t url;
    S3Headers form_data;
    s3_error_t error;
} s3_presigned_post_t;

CXX_C_API s3_presigned_post_t s3_get_presigned_post_form_data(
    s3_client_t* client, const char* bucket, const char* object,
    int expires_secs);

CXX_C_API void s3_presigned_post_free(s3_presigned_post_t* p);

// ── Object Tags ──

CXX_C_API s3_tag_set_t s3_get_object_tags(s3_client_t* client, const char* bucket, const char* object, s3_error_t* err);
CXX_C_API s3_error_t   s3_set_object_tags(s3_client_t* client, const char* bucket, const char* object, const s3_tag_set_t* tags);
CXX_C_API s3_error_t   s3_delete_object_tags(s3_client_t* client, const char* bucket, const char* object);

// ── Object Retention ──

CXX_C_API s3_object_retention_t s3_get_object_retention(s3_client_t* client, const char* bucket, const char* object, s3_error_t* err);
CXX_C_API s3_error_t            s3_set_object_retention(s3_client_t* client, const char* bucket, const char* object, const s3_object_retention_t* retention);

// ── Legal Hold ──

CXX_C_API int           s3_is_object_legal_hold_enabled(s3_client_t* client, const char* bucket, const char* object, s3_error_t* err);
CXX_C_API s3_error_t s3_enable_object_legal_hold(s3_client_t* client, const char* bucket, const char* object);
CXX_C_API s3_error_t s3_disable_object_legal_hold(s3_client_t* client, const char* bucket, const char* object);

// ── SelectObjectContent ──

typedef struct {
    tstr_t expression;
    tstr_t input_format;   // "CSV", "JSON", "Parquet"
    tstr_t output_format;  // "CSV", "JSON"
    tstr_t csv_delimiter;
    tstr_t csv_quote_char;
    tstr_t csv_header_info; // "USE", "IGNORE", "NONE"
} s3_select_request_t;

CXX_C_API s3_error_t s3_select_object_content(
    s3_client_t* client, const char* bucket, const char* object,
    const s3_select_request_t* request,
    s3_data_callback_t callback, void* userdata);

CXX_C_API void s3_select_request_free(s3_select_request_t* req);

#ifdef __cplusplus
}
#endif

#endif // S3_CLIENT_H
