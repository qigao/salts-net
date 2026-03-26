#include "s3/s3_multipart.h"
#include "s3_client_internal.h"
#include "s3_http.h"
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── XML Parsing Helpers ───────────────────────────────────────────── */

static char *extract_xml_tag(const char *xml, const char *tag) {
  if (!xml || !tag) return NULL;

  char open_tag[128];
  char close_tag[128];
  fmt(open_tag, sizeof(open_tag), "<{}>", tag);
  fmt(close_tag, sizeof(close_tag), "</{}>", tag);

  const char *start = strstr(xml, open_tag);
  if (!start) return NULL;
  start += strlen(open_tag);

  const char *end = strstr(start, close_tag);
  if (!end) return NULL;

  size_t len = end - start;
  char *result = malloc(len + 1);
  if (!result) return NULL;

  memcpy(result, start, len);
  result[len] = '\0';
  return result;
}

/* ── Initiate ──────────────────────────────────────────────────────── */

s3_multipart_upload_t *s3_multipart_initiate(
    s3_client_t *client,
    const char *bucket,
    const char *key,
    S3Headers *metadata,
    s3_error_t *error) {

  if (!client || !bucket || !key) {
    if (error) *error = s3_error_make(-1, "Invalid parameters");
    return NULL;
  }

  // Build URI: /{bucket}/{key}?uploads
  char uri[2048];
  fmt(uri, sizeof(uri), "/{}/{}", bucket, key);

  S3Headers query = S3Headers_init();
  s3_headers_add(&query, "uploads", "");

  S3Headers headers = S3Headers_init();
  if (metadata) {
    c_foreach(i, S3Headers, *metadata) {
      s3_headers_add(&headers, cstr_str(&i.ref->first), cstr_str(&i.ref->second));
    }
  }

  s3_http_response_t resp = s3_execute_signed(client, "POST", uri,
                                               &headers, &query, NULL, 0);

  S3Headers_drop(&headers);
  S3Headers_drop(&query);

  if (!s3_is_ok(resp.error) || resp.status_code != 200) {
    if (error) *error = resp.error;
    s3_http_response_free(&resp);
    return NULL;
  }

  // Parse UploadId from XML response
  char *upload_id = extract_xml_tag(resp.body, "UploadId");
  s3_http_response_free(&resp);

  if (!upload_id) {
    if (error) *error = s3_error_make(-1, "Failed to parse UploadId from response");
    return NULL;
  }

  // Create handle
  s3_multipart_upload_t *upload = calloc(1, sizeof(s3_multipart_upload_t));
  if (!upload) {
    free(upload_id);
    if (error) *error = s3_error_make(-1, "Out of memory");
    return NULL;
  }

  upload->upload_id = upload_id;
  upload->bucket = strdup(bucket);
  upload->key = strdup(key);
  upload->part_capacity = 16;
  upload->parts = calloc(upload->part_capacity, sizeof(s3_part_info_t));

  if (error) *error = S3_OK;
  return upload;
}

/* ── Upload Part ───────────────────────────────────────────────────── */

int s3_multipart_upload_part(
    s3_client_t *client,
    s3_multipart_upload_t *upload,
    int part_number,
    const char *data,
    size_t data_len,
    s3_error_t *error) {

  if (!client || !upload || !data || part_number < 1 || part_number > S3_MAX_PARTS) {
    if (error) *error = s3_error_make(-1, "Invalid parameters");
    return -1;
  }

  // Validate part size (except last part can be smaller)
  if (data_len > S3_MAX_PART_SIZE) {
    if (error) *error = s3_error_make(-1, "Part size exceeds 5GB limit");
    return -1;
  }

  // Build URI: /{bucket}/{key}?partNumber={n}&uploadId={id}
  char uri[2048];
  fmt(uri, sizeof(uri), "/{}/{}", upload->bucket, upload->key);

  S3Headers query = S3Headers_init();
  char part_str[16];
  fmt(part_str, sizeof(part_str), "{}", part_number);
  s3_headers_add(&query, "partNumber", part_str);
  s3_headers_add(&query, "uploadId", upload->upload_id);

  s3_http_response_t resp = s3_execute_signed(client, "PUT", uri,
                                               NULL, &query, data, data_len);

  S3Headers_drop(&query);

  if (!s3_is_ok(resp.error) || resp.status_code != 200) {
    if (error) *error = resp.error;
    s3_http_response_free(&resp);
    return -1;
  }

  // Extract ETag from response headers
  const char *etag = s3_headers_get(&resp.headers, "ETag");
  if (!etag) {
    if (error) *error = s3_error_make(-1, "Missing ETag in response");
    s3_http_response_free(&resp);
    return -1;
  }

  // Strip quotes from ETag if present
  char *etag_clean = strdup(etag);
  if (etag_clean[0] == '"') {
    size_t len = strlen(etag_clean);
    if (len > 2 && etag_clean[len - 1] == '"') {
      memmove(etag_clean, etag_clean + 1, len - 2);
      etag_clean[len - 2] = '\0';
    }
  }

  // Store part info
  if (upload->part_count >= upload->part_capacity) {
    upload->part_capacity *= 2;
    upload->parts = realloc(upload->parts, upload->part_capacity * sizeof(s3_part_info_t));
  }

  upload->parts[upload->part_count].part_number = part_number;
  upload->parts[upload->part_count].etag = etag_clean;
  upload->part_count++;

  s3_http_response_free(&resp);
  if (error) *error = S3_OK;
  return 0;
}

/* ── Complete ──────────────────────────────────────────────────────── */

s3_error_t s3_multipart_complete(
    s3_client_t *client,
    s3_multipart_upload_t *upload) {

  if (!client || !upload) {
    return s3_error_make(-1, "Invalid parameters");
  }

  // Build XML body with part list
  tstr_t xml = tstr_new();
  xml = tstr_cat(xml, "<CompleteMultipartUpload>");

  for (int i = 0; i < upload->part_count; i++) {
    xml = tstr_cat(xml, "<Part>");
    xml = tstr_cat_fmt(xml, "<PartNumber>%d</PartNumber>", upload->parts[i].part_number);
    xml = tstr_cat_fmt(xml, "<ETag>%s</ETag>", upload->parts[i].etag);
    xml = tstr_cat(xml, "</Part>");
  }

  xml = tstr_cat(xml, "</CompleteMultipartUpload>");

  // Build URI: /{bucket}/{key}?uploadId={id}
  char uri[2048];
  fmt(uri, sizeof(uri), "/{}/{}", upload->bucket, upload->key);

  S3Headers query = S3Headers_init();
  s3_headers_add(&query, "uploadId", upload->upload_id);

  s3_http_response_t resp = s3_execute_signed(client, "POST", uri,
                                               NULL, &query, xml, tstr_len(xml));

  S3Headers_drop(&query);
  tstr_free(xml);

  s3_error_t result = resp.error;
  if (s3_is_ok(result) && resp.status_code != 200) {
    result = s3_error_make(resp.status_code, "Complete multipart failed");
  }

  s3_http_response_free(&resp);
  return result;
}

/* ── Abort ─────────────────────────────────────────────────────────── */

s3_error_t s3_multipart_abort(
    s3_client_t *client,
    s3_multipart_upload_t *upload) {

  if (!client || !upload) {
    return s3_error_make(-1, "Invalid parameters");
  }

  // Build URI: /{bucket}/{key}?uploadId={id}
  char uri[2048];
  fmt(uri, sizeof(uri), "/{}/{}", upload->bucket, upload->key);

  S3Headers query = S3Headers_init();
  s3_headers_add(&query, "uploadId", upload->upload_id);

  s3_http_response_t resp = s3_execute_signed(client, "DELETE", uri,
                                               NULL, &query, NULL, 0);

  S3Headers_drop(&query);

  s3_error_t result = resp.error;
  if (s3_is_ok(result) && resp.status_code != 204) {
    result = s3_error_make(resp.status_code, "Abort multipart failed");
  }

  s3_http_response_free(&resp);
  return result;
}

/* ── Cleanup ───────────────────────────────────────────────────────── */

void s3_multipart_free(s3_multipart_upload_t *upload) {
  if (!upload) return;

  free(upload->upload_id);
  free(upload->bucket);
  free(upload->key);

  for (int i = 0; i < upload->part_count; i++) {
    free(upload->parts[i].etag);
  }
  free(upload->parts);

  free(upload);
}

/* ── High-Level API ────────────────────────────────────────────────── */

s3_error_t s3_put_object_multipart(
    s3_client_t *client,
    const char *bucket,
    const char *key,
    const char *data,
    size_t data_len,
    size_t part_size,
    S3Headers *metadata) {

  if (!client || !bucket || !key || !data) {
    return s3_error_make(-1, "Invalid parameters");
  }

  // Default part size: 5MB
  if (part_size == 0) part_size = S3_MIN_PART_SIZE;

  // Validate part size
  if (part_size < S3_MIN_PART_SIZE || part_size > S3_MAX_PART_SIZE) {
    return s3_error_make(-1, "Part size must be between 5MB and 5GB");
  }

  // Calculate number of parts
  size_t num_parts = (data_len + part_size - 1) / part_size;
  if (num_parts > S3_MAX_PARTS) {
    return s3_error_make(-1, "Too many parts (max 10000)");
  }

  // Initiate
  s3_error_t error;
  s3_multipart_upload_t *upload = s3_multipart_initiate(client, bucket, key, metadata, &error);
  if (!upload) return error;

  // Upload parts
  for (size_t i = 0; i < num_parts; i++) {
    size_t offset = i * part_size;
    size_t chunk_size = (offset + part_size > data_len) ? (data_len - offset) : part_size;

    int ret = s3_multipart_upload_part(client, upload, (int)(i + 1),
                                       data + offset, chunk_size, &error);
    if (ret != 0) {
      s3_multipart_abort(client, upload);
      s3_multipart_free(upload);
      return error;
    }
  }

  // Complete
  error = s3_multipart_complete(client, upload);
  s3_multipart_free(upload);

  return error;
}
