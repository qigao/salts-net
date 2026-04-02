/**
 * @file mime_parser.h
 * @brief MIME message parser with callback-driven state machine
 *
 * Design philosophy (inspired by llhttp):
 * - Zero-copy: parser holds pointers to original data
 * - Callback-driven: no special cases, unified interface
 * - State machine: clean, predictable parsing
 * - Memory pool: all allocations from mem_pool_t
 *
 * Usage:
 *   mime_parser_t parser;
 *   mime_settings_t settings = {0};
 *   settings.on_header_field = my_header_field_cb;
 *   settings.on_body = my_body_cb;
 *
 *   mime_parser_init(&parser, &settings, pool);
 *   mime_errno_t err = mime_parse(&parser, data, len);
 */

#ifndef MIME_PARSER_H
#define MIME_PARSER_H

#include "platform.h"
#include <stddef.h>
#include <stdint.h>
#include "turbo_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error codes ───────────────────────────────────────────────────── */

typedef enum {
  MIME_OK = 0,
  MIME_ERROR_INVALID_HEADER,
  MIME_ERROR_INVALID_BOUNDARY,
  MIME_ERROR_MISSING_BOUNDARY,
  MIME_ERROR_NESTED_TOO_DEEP,
  MIME_ERROR_MEMORY,
  MIME_ERROR_CALLBACK_FAILED
} mime_errno_t;

/* ── Parser state ──────────────────────────────────────────────────── */

typedef enum {
  MIME_STATE_HEADERS,
  MIME_STATE_HEADERS_DONE,
  MIME_STATE_BODY,
  MIME_STATE_BOUNDARY,
  MIME_STATE_PART_BEGIN,
  MIME_STATE_COMPLETE
} mime_state_t;

/* ── Forward declarations ──────────────────────────────────────────── */

typedef struct mime_parser_s mime_parser_t;
typedef struct mime_settings_s mime_settings_t;

/* ── Callbacks ─────────────────────────────────────────────────────── */

/**
 * Callback signatures (llhttp-style)
 * Return 0 to continue, non-zero to abort parsing
 */
typedef int (*mime_data_cb)(mime_parser_t *parser, const char *data, size_t len);
typedef int (*mime_cb)(mime_parser_t *parser);

struct mime_settings_s {
  mime_data_cb on_header_field;      /* Called when header name is parsed */
  mime_data_cb on_header_value;      /* Called when header value is parsed */
  mime_cb on_headers_complete;       /* Called after all headers parsed */
  mime_data_cb on_body;              /* Called with body data chunks */
  mime_cb on_part_begin;             /* Called when new multipart part starts */
  mime_cb on_part_complete;          /* Called when part ends */
  mime_cb on_message_complete;       /* Called when entire message parsed */
};

/* ── Parser instance ───────────────────────────────────────────────── */

#define MIME_MAX_BOUNDARY_LEN 70
#define MIME_MAX_NESTING 8

struct mime_parser_s {
  /* State */
  mime_state_t state;
  mime_errno_t error;

  /* Settings */
  mime_settings_t settings;

  /* Memory */
  mem_pool_t *pool;

  /* Boundary tracking (for multipart) */
  char boundary[MIME_MAX_BOUNDARY_LEN + 1];
  size_t boundary_len;
  int nesting_level;

  /* Parsing context */
  const char *current_field;
  size_t current_field_len;

  /* User data */
  void *data;
};

/* ── Core API ──────────────────────────────────────────────────────── */

/**
 * Initialize parser with settings and memory pool
 */
CXX_C_API void mime_parser_init(mime_parser_t *parser,
                                 const mime_settings_t *settings,
                                 mem_pool_t *pool);

/**
 * Parse MIME data (can be called multiple times for streaming)
 */
CXX_C_API mime_errno_t mime_parse(mime_parser_t *parser,
                                   const char *data,
                                   size_t len);

/**
 * Reset parser for reuse
 */
CXX_C_API void mime_parser_reset(mime_parser_t *parser);

/**
 * Get error message for error code
 */
CXX_C_API const char *mime_errno_name(mime_errno_t err);

/* ── Helper API ────────────────────────────────────────────────────── */

/**
 * Extract boundary from Content-Type header value
 * Example: "multipart/mixed; boundary=----Boundary123"
 * Returns pointer to boundary string (not null-terminated), sets *len
 * Returns NULL if no boundary found
 */
CXX_C_API const char *mime_extract_boundary(const char *content_type,
                                             size_t content_type_len,
                                             size_t *boundary_len);

/**
 * Check if content type is multipart
 */
CXX_C_API int mime_is_multipart(const char *content_type, size_t len);

/**
 * Find boundary in data buffer
 * Returns offset of boundary start, or -1 if not found
 */
CXX_C_API int mime_find_boundary(const char *data, size_t len,
                                  const char *boundary, size_t boundary_len);

#ifdef __cplusplus
}
#endif

#endif /* MIME_PARSER_H */
