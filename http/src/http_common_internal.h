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
};

/* ── Cookie internals ────────────────────────────────────────────── */

typedef struct http_cookie_s {
  tstr_t name;
  tstr_t value;
  tstr_t domain;
  tstr_t path;
  int secure;
  int http_only;
  struct http_cookie_s *next;
} http_cookie_t;

struct http_cookie_jar_s {
  http_cookie_t *cookies;
  int count;
};

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
};

#endif /* HTTP_COMMON_INTERNAL_H */
