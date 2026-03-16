#ifndef S3_CLIENT_H
#define S3_CLIENT_H

#include <turbo_str.h>
#include <platform.h>
#include <CoroNet/turbo_coro_context.h>
#include "s3_types.h"
#include "s3_credentials.h"
#include "s3_error.h"
#include "s3_response.h"
#include "s3_sse.h"

/* Forward declare http_progress_cb to avoid circular dependency */
typedef void (*http_progress_cb)(size_t downloaded, size_t total, void *user_data);

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
CXX_C_API s3_client_t* s3_client_create(coro_context_t *ctx,
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

/**
 * @brief Upload file to S3 with streaming (low memory usage, progress support)
 * @param client S3 client
 * @param bucket Bucket name
 * @param object Object key
 * @param file_path Local file path to upload
 * @param content_type Content type (e.g., "application/octet-stream", can be NULL)
 * @param progress_cb Progress callback (can be NULL)
 * @param progress_ud User data for progress callback
 * @return S3_OK on success, error code otherwise
 */
CXX_C_API s3_error_t s3_put_object_from_file(s3_client_t* client,
                                             const char* bucket, const char* object,
                                             const char* file_path,
                                             const char* content_type,
                                             http_progress_cb progress_cb,
                                             void* progress_ud);

CXX_C_API s3_error_t s3_download_object(s3_client_t* client,
                                   const char* bucket, const char* object,
                                   const char* filename);

/**
 * @brief Download S3 object to file with streaming (low memory usage, progress support)
 * @param client S3 client
 * @param bucket Bucket name
 * @param object Object key
 * @param output_path Local file path to save
 * @param progress_cb Progress callback (can be NULL)
 * @param progress_ud User data for progress callback
 * @return S3_OK on success, error code otherwise
 */
CXX_C_API s3_error_t s3_download_object_stream(s3_client_t* client,
                                               const char* bucket, const char* object,
                                               const char* output_path,
                                               http_progress_cb progress_cb,
                                               void* progress_ud);

/* ── Batch Operations ─────────────────────────────────────────────── */

/**
 * @brief Batch upload/download item
 */
typedef struct {
    const char* bucket;
    const char* key;
    const char* file_path;      /* Local file path (upload source or download target) */
    const char* content_type;   /* Content type for upload (can be NULL) */
} s3_batch_item_t;

/**
 * @brief Batch operation result
 */
typedef struct {
    s3_error_t error;
    int index;                  /* Index in original batch array */
} s3_batch_result_t;

/**
 * @brief Batch progress information
 */
typedef struct {
    int current_file;           /* Current file being processed (1-based) */
    int total_files;            /* Total number of files */
    size_t current_file_uploaded;  /* Bytes uploaded for current file */
    size_t current_file_size;      /* Size of current file */
    size_t total_uploaded;      /* Total bytes uploaded across all files */
    size_t total_size;          /* Total size of all files */
    int completed_files;        /* Number of completed files */
    int failed_files;           /* Number of failed files */
} s3_batch_progress_t;

/**
 * @brief Batch progress callback
 */
typedef void (*s3_batch_progress_cb)(const s3_batch_progress_t* progress, void* user_data);

/**
 * @brief Batch upload files to S3 with concurrency
 * @param client S3 client
 * @param items Array of items to upload
 * @param count Number of items
 * @param concurrency Max concurrent uploads (0 = default 10)
 * @return Array of results (caller must free with s3_batch_results_free)
 */
CXX_C_API s3_batch_result_t* s3_put_objects_batch(s3_client_t* client,
                                                  const s3_batch_item_t* items,
                                                  int count,
                                                  int concurrency);

/**
 * @brief Batch upload files to S3 with concurrency and progress tracking
 * @param client S3 client
 * @param items Array of items to upload
 * @param count Number of items
 * @param concurrency Max concurrent uploads (0 = default 10)
 * @param progress_cb Progress callback (can be NULL)
 * @param progress_ud User data for progress callback
 * @return Array of results (caller must free with s3_batch_results_free)
 */
CXX_C_API s3_batch_result_t* s3_put_objects_batch_progress(
    s3_client_t* client,
    const s3_batch_item_t* items,
    int count,
    int concurrency,
    s3_batch_progress_cb progress_cb,
    void* progress_ud
);

/**
 * @brief Batch download files from S3 with concurrency
 * @param client S3 client
 * @param items Array of items to download
 * @param count Number of items
 * @param concurrency Max concurrent downloads (0 = default 10)
 * @return Array of results (caller must free with s3_batch_results_free)
 */
CXX_C_API s3_batch_result_t* s3_get_objects_batch(s3_client_t* client,
                                                  const s3_batch_item_t* items,
                                                  int count,
                                                  int concurrency);

/**
 * @brief Free batch results
 * @param results Results array from s3_put_objects_batch or s3_get_objects_batch
 * @param count Number of results
 */
CXX_C_API void s3_batch_results_free(s3_batch_result_t* results, int count);

/* ── Multipart Upload (Large Files >5GB) ──────────────────────────── */

/**
 * @brief Multipart upload progress callback
 * @param part_number Current part number (1-based)
 * @param total_parts Total number of parts
 * @param part_uploaded Bytes uploaded in current part
 * @param part_size Size of current part
 * @param total_uploaded Total bytes uploaded so far
 * @param total_size Total file size
 * @param user_data User data
 */
typedef void (*s3_multipart_progress_cb)(
    int part_number, int total_parts,
    size_t part_uploaded, size_t part_size,
    size_t total_uploaded, size_t total_size,
    void* user_data
);

/**
 * @brief Multipart upload options
 */
typedef struct {
    size_t part_size_mb;            /* Part size in MB (5-100, default 10) */
    int concurrency;                /* Max concurrent part uploads (default 10) */
    s3_multipart_progress_cb progress_cb;  /* Progress callback (can be NULL) */
    void* progress_ud;              /* User data for progress callback */
    const char* resume_file;        /* Resume file path (can be NULL) */
} s3_multipart_options_t;

/**
 * @brief Upload large file with multipart upload (supports >5GB files)
 * @param client S3 client
 * @param bucket Bucket name
 * @param key Object key
 * @param file_path Local file path
 * @param content_type Content type (can be NULL)
 * @param options Multipart options (can be NULL for defaults)
 * @return S3_OK on success, error code otherwise
 *
 * Features:
 * - Supports files >5GB (up to 5TB)
 * - Concurrent part uploads (10x faster)
 * - Resume support (saves progress to resume_file)
 * - Progress tracking per part and overall
 */
CXX_C_API s3_error_t s3_put_object_multipart_file(
    s3_client_t* client,
    const char* bucket,
    const char* key,
    const char* file_path,
    const char* content_type,
    const s3_multipart_options_t* options
);

/**
 * @brief Abort multipart upload (cleanup incomplete upload)
 * @param client S3 client
 * @param bucket Bucket name
 * @param key Object key
 * @param upload_id Upload ID from CreateMultipartUpload
 * @return S3_OK on success, error code otherwise
 */
CXX_C_API s3_error_t s3_abort_multipart_upload(
    s3_client_t* client,
    const char* bucket,
    const char* key,
    const char* upload_id
);

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
