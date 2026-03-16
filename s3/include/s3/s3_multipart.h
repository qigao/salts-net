/**
 * @file s3_multipart.h
 * @brief S3 Multipart Upload API
 *
 * Implements AWS S3 multipart upload protocol:
 * 1. InitiateMultipartUpload - Get UploadId
 * 2. UploadPart (multiple) - Upload chunks, collect ETags
 * 3. CompleteMultipartUpload - Commit with ETag list
 * 4. AbortMultipartUpload - Cancel incomplete upload
 */

#ifndef S3_MULTIPART_H
#define S3_MULTIPART_H

#include "platform.h"
#include "s3_client.h"
#include "s3_error.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ─────────────────────────────────────────────────────── */

#define S3_MIN_PART_SIZE (5 * 1024 * 1024)      // 5MB minimum per part
#define S3_MAX_PART_SIZE (5LL * 1024 * 1024 * 1024) // 5GB maximum per part
#define S3_MAX_PARTS 10000                       // AWS limit

/* ── Part Info ─────────────────────────────────────────────────────── */

typedef struct {
  int part_number;      // 1-based part number
  char *etag;           // ETag from UploadPart response (must free)
} s3_part_info_t;

/* ── Multipart Upload Handle ───────────────────────────────────────── */

typedef struct {
  char *upload_id;      // UploadId from InitiateMultipartUpload
  char *bucket;         // Bucket name (copied)
  char *key;            // Object key (copied)
  s3_part_info_t *parts; // Array of uploaded parts
  int part_count;       // Number of parts uploaded
  int part_capacity;    // Allocated capacity
} s3_multipart_upload_t;

/* ── Initiate ──────────────────────────────────────────────────────── */

/**
 * Start multipart upload
 * Returns upload handle (caller must free with s3_multipart_free)
 */
CXX_C_API s3_multipart_upload_t *s3_multipart_initiate(
    s3_client_t *client,
    const char *bucket,
    const char *key,
    S3Headers *metadata,
    s3_error_t *error);

/* ── Upload Part ───────────────────────────────────────────────────── */

/**
 * Upload single part
 * part_number: 1-based index (1 to 10000)
 * data: Part data
 * data_len: Part size (5MB to 5GB, except last part can be smaller)
 * Returns 0 on success, -1 on failure
 */
CXX_C_API int s3_multipart_upload_part(
    s3_client_t *client,
    s3_multipart_upload_t *upload,
    int part_number,
    const char *data,
    size_t data_len,
    s3_error_t *error);

/* ── Complete ──────────────────────────────────────────────────────── */

/**
 * Complete multipart upload
 * Sends XML with all part ETags to finalize object
 */
CXX_C_API s3_error_t s3_multipart_complete(
    s3_client_t *client,
    s3_multipart_upload_t *upload);

/* ── Abort ─────────────────────────────────────────────────────────── */

/**
 * Abort multipart upload
 * Cancels upload and frees server-side resources
 */
CXX_C_API s3_error_t s3_multipart_abort(
    s3_client_t *client,
    s3_multipart_upload_t *upload);

/* ── Cleanup ───────────────────────────────────────────────────────── */

/**
 * Free multipart upload handle
 */
CXX_C_API void s3_multipart_free(s3_multipart_upload_t *upload);

/* ── High-Level API ────────────────────────────────────────────────── */

/**
 * Upload large object with automatic chunking
 * Automatically splits data into parts and handles complete/abort
 * part_size: Size per part (default 5MB if 0)
 */
CXX_C_API s3_error_t s3_put_object_multipart(
    s3_client_t *client,
    const char *bucket,
    const char *key,
    const char *data,
    size_t data_len,
    size_t part_size,
    S3Headers *metadata);

#ifdef __cplusplus
}
#endif

#endif /* S3_MULTIPART_H */
