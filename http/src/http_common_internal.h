#ifndef HTTP_COMMON_INTERNAL_H
#define HTTP_COMMON_INTERNAL_H

/**
 * @file http_common_internal.h
 * @brief Internal struct definitions for shared HTTP types.
 *
 * This header is PRIVATE — only included by .c files that need
 * direct field access to params, cookies, or multipart structs.
 */

#include "http_common.h"
#include "turbo_str.h"
#include <turbo_fs.h>

/* ── URL params internals ────────────────────────────────────────── */

struct http_param_entry {
  tstr_t key;
  tstr_t value;
  struct http_param_entry *next;
};

struct http_params_s {
  struct http_param_entry *head;
  int count;
  http_error_code_t error_code;
};

/* ── Cookie internals ────────────────────────────────────────────── */
/* Uses the full cookie parser types with domain matching, expiry, etc. */

#include "cookie_jar.h"

/* ── Multipart internals ─────────────────────────────────────────── */

typedef struct http_multipart_file_stream_s {
  tstr_t file_path;
  turbo_file_t fd;
  int64_t file_size;
  int64_t offset;
  char chunk_buf[16384];
} http_multipart_file_stream_t;

typedef struct http_multipart_part_s {
  tstr_t name;
  tstr_t filename;
  tstr_t content_type;
  tstr_t value;
  void *data;
  size_t data_len;
  int is_file;
  int is_stream;
  http_multipart_file_stream_t *stream_ctx;
  struct http_multipart_part_s *next;
} http_multipart_part_t;

struct http_multipart_form_s {
  http_multipart_part_t *parts;
  char boundary[48];
  int part_count;
  http_error_code_t error_code;
};

#endif /* HTTP_COMMON_INTERNAL_H */
