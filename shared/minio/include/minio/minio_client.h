#ifndef MINIO_CLIENT_H
#define MINIO_CLIENT_H

#include <turbo_str.h>
#include <platform.h>
#include <http_client.h>
#include "minio_types.h"
#include "minio_credentials.h"
#include "minio_error.h"
#include "minio_response.h"
#include "minio_sse.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct minio_client_s minio_client_t;

typedef struct {
    tstr_t host;
    int    port;
    int    is_https;
    tstr_t region;
    int    virtual_style; // 1 for virtual-hosted, 0 for path-style
} minio_base_url_t;

CXX_C_API void minio_base_url_free(minio_base_url_t* url);

// Client lifecycle
CXX_C_API minio_client_t* minio_client_create(const minio_base_url_t* base_url,
                                   minio_credential_provider_t* provider);
CXX_C_API void            minio_client_destroy(minio_client_t* client);

// ── Bucket operations ──

CXX_C_API minio_error_t minio_make_bucket(minio_client_t* client, const char* bucket, const char* region);
CXX_C_API minio_error_t minio_remove_bucket(minio_client_t* client, const char* bucket);
CXX_C_API int           minio_bucket_exists(minio_client_t* client, const char* bucket, minio_error_t* err);

typedef struct {
    MinioBucketVec  buckets;
    minio_error_t   error;
} minio_list_buckets_response_t;

CXX_C_API minio_list_buckets_response_t minio_list_buckets(minio_client_t* client);
CXX_C_API void minio_list_buckets_free(minio_list_buckets_response_t* resp);

// ── Object CRUD ──

CXX_C_API minio_error_t minio_upload_object(minio_client_t* client,
                                 const char* bucket, const char* object,
                                 const char* filename);

CXX_C_API minio_error_t minio_put_object(minio_client_t* client,
                              const char* bucket, const char* object,
                              const char* data, size_t len,
                              const char* content_type);

CXX_C_API minio_error_t minio_download_object(minio_client_t* client,
                                   const char* bucket, const char* object,
                                   const char* filename);

typedef int (*minio_data_callback_t)(const char* data, size_t len, void* userdata);

CXX_C_API minio_error_t minio_get_object(minio_client_t* client,
                              const char* bucket, const char* object,
                              minio_data_callback_t callback, void* userdata);

typedef struct {
    tstr_t  etag;
    size_t  size;
    time_t  last_modified;
    tstr_t  content_type;
    minio_error_t error;
} minio_stat_object_response_t;

CXX_C_API void minio_stat_object_response_free(minio_stat_object_response_t* resp);

CXX_C_API minio_stat_object_response_t minio_stat_object(minio_client_t* client,
                                              const char* bucket,
                                              const char* object);

CXX_C_API minio_error_t minio_remove_object(minio_client_t* client,
                                 const char* bucket, const char* object);

// List objects (iterator-based)
typedef struct minio_list_objects_iter_s minio_list_objects_iter_t;

CXX_C_API minio_list_objects_iter_t* minio_list_objects(minio_client_t* client,
                                            const char* bucket,
                                            const char* prefix,
                                            int recursive);
CXX_C_API int           minio_list_objects_next(minio_list_objects_iter_t* iter, minio_item_t* item);
CXX_C_API minio_error_t minio_list_objects_error(minio_list_objects_iter_t* iter);
CXX_C_API void          minio_list_objects_free(minio_list_objects_iter_t* iter);

// ── Multipart Upload ──

typedef struct {
    tstr_t upload_id;
    minio_error_t error;
} minio_create_multipart_response_t;

typedef struct {
    tstr_t etag;
    minio_error_t error;
} minio_upload_part_response_t;

typedef struct {
    tstr_t etag;
    tstr_t location;
    minio_error_t error;
} minio_complete_multipart_response_t;

CXX_C_API minio_create_multipart_response_t minio_create_multipart_upload(
    minio_client_t* client, const char* bucket, const char* object,
    const char* content_type, const minio_sse_t* sse);

CXX_C_API minio_upload_part_response_t minio_upload_part(
    minio_client_t* client, const char* bucket, const char* object,
    const char* upload_id, int part_number,
    const char* data, size_t len);

CXX_C_API minio_upload_part_response_t minio_upload_part_copy(
    minio_client_t* client, const char* bucket, const char* object,
    const char* upload_id, int part_number,
    const char* src_bucket, const char* src_object,
    size_t offset, size_t length);

CXX_C_API minio_complete_multipart_response_t minio_complete_multipart_upload(
    minio_client_t* client, const char* bucket, const char* object,
    const char* upload_id, const char** etags, int part_count);

CXX_C_API minio_error_t minio_abort_multipart_upload(
    minio_client_t* client, const char* bucket, const char* object,
    const char* upload_id);

CXX_C_API void minio_create_multipart_response_free(minio_create_multipart_response_t* resp);
CXX_C_API void minio_upload_part_response_free(minio_upload_part_response_t* resp);
CXX_C_API void minio_complete_multipart_response_free(minio_complete_multipart_response_t* resp);

// ── Copy / Compose ──

typedef struct {
    tstr_t etag;
    time_t last_modified;
    minio_error_t error;
} minio_copy_object_response_t;

CXX_C_API minio_copy_object_response_t minio_copy_object(
    minio_client_t* client,
    const char* src_bucket, const char* src_object,
    const char* dst_bucket, const char* dst_object);

typedef struct {
    const char* bucket;
    const char* object;
    size_t offset;
    size_t length;
} minio_compose_source_t;

CXX_C_API minio_copy_object_response_t minio_compose_object(
    minio_client_t* client,
    const char* bucket, const char* object,
    const minio_compose_source_t* sources, int source_count);

// ── Batch Delete ──

typedef struct {
    minio_delete_error_t* errors;
    int error_count;
    minio_error_t error;
} minio_remove_objects_response_t;

CXX_C_API minio_remove_objects_response_t minio_remove_objects(
    minio_client_t* client, const char* bucket,
    const char** keys, int key_count);

CXX_C_API void minio_remove_objects_response_free(minio_remove_objects_response_t* resp);

// ── Presigned URLs ──

CXX_C_API tstr_t minio_get_presigned_object_url(
    minio_client_t* client, const char* method,
    const char* bucket, const char* object,
    int expires_secs);

typedef struct {
    tstr_t url;
    MinioHeaders form_data;
    minio_error_t error;
} minio_presigned_post_t;

CXX_C_API minio_presigned_post_t minio_get_presigned_post_form_data(
    minio_client_t* client, const char* bucket, const char* object,
    int expires_secs);

CXX_C_API void minio_presigned_post_free(minio_presigned_post_t* p);

// ── Object Tags ──

CXX_C_API minio_tag_set_t minio_get_object_tags(minio_client_t* client, const char* bucket, const char* object, minio_error_t* err);
CXX_C_API minio_error_t   minio_set_object_tags(minio_client_t* client, const char* bucket, const char* object, const minio_tag_set_t* tags);
CXX_C_API minio_error_t   minio_delete_object_tags(minio_client_t* client, const char* bucket, const char* object);

// ── Object Retention ──

CXX_C_API minio_object_retention_t minio_get_object_retention(minio_client_t* client, const char* bucket, const char* object, minio_error_t* err);
CXX_C_API minio_error_t            minio_set_object_retention(minio_client_t* client, const char* bucket, const char* object, const minio_object_retention_t* retention);

// ── Legal Hold ──

CXX_C_API int           minio_is_object_legal_hold_enabled(minio_client_t* client, const char* bucket, const char* object, minio_error_t* err);
CXX_C_API minio_error_t minio_enable_object_legal_hold(minio_client_t* client, const char* bucket, const char* object);
CXX_C_API minio_error_t minio_disable_object_legal_hold(minio_client_t* client, const char* bucket, const char* object);

// ── SelectObjectContent ──

typedef struct {
    tstr_t expression;
    tstr_t input_format;   // "CSV", "JSON", "Parquet"
    tstr_t output_format;  // "CSV", "JSON"
    tstr_t csv_delimiter;
    tstr_t csv_quote_char;
    tstr_t csv_header_info; // "USE", "IGNORE", "NONE"
} minio_select_request_t;

CXX_C_API minio_error_t minio_select_object_content(
    minio_client_t* client, const char* bucket, const char* object,
    const minio_select_request_t* request,
    minio_data_callback_t callback, void* userdata);

CXX_C_API void minio_select_request_free(minio_select_request_t* req);

#ifdef __cplusplus
}
#endif

#endif // MINIO_CLIENT_H
