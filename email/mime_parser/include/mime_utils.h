/**
 * @file mime_utils.h
 * @brief MIME utility functions for encoding/decoding
 */

#ifndef MIME_UTILS_H
#define MIME_UTILS_H

#include "platform.h"
#include <stddef.h>
#include <stdint.h>
#include "turbo_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Content-Transfer-Encoding types ───────────────────────────────── */

typedef enum {
  MIME_ENCODING_7BIT,
  MIME_ENCODING_8BIT,
  MIME_ENCODING_BINARY,
  MIME_ENCODING_QUOTED_PRINTABLE,
  MIME_ENCODING_BASE64,
  MIME_ENCODING_UNKNOWN
} mime_encoding_t;

/* ── Encoding detection ────────────────────────────────────────────── */

/**
 * Parse Content-Transfer-Encoding header value
 */
mime_encoding_t mime_parse_encoding(const char *encoding_str, size_t len);

/**
 * Get encoding name string
 */
const char *mime_encoding_name(mime_encoding_t encoding);

/* ── Decoding functions ────────────────────────────────────────────── */

/**
 * Decode body based on Content-Transfer-Encoding
 * Allocates output buffer using pool
 * Returns 0 on success, -1 on error
 */
int mime_decode_body(mem_pool_t *pool,
                                const char *input, size_t input_len,
                                mime_encoding_t encoding,
                                char **output, size_t *output_len);

/**
 * Decode Base64 content (wrapper around base64_utils)
 */
int mime_decode_base64(mem_pool_t *pool,
                                  const char *input, size_t input_len,
                                  uint8_t **output, size_t *output_len);

/**
 * Decode Quoted-Printable content
 */
int mime_decode_quoted_printable(mem_pool_t *pool,
                                            const char *input, size_t input_len,
                                            char **output, size_t *output_len);

/* ── Content-Type parsing ──────────────────────────────────────────── */

typedef struct {
  const char *type;        // e.g., "text"
  size_t type_len;
  const char *subtype;     // e.g., "plain"
  size_t subtype_len;
  const char *charset;     // e.g., "utf-8" (NULL if not specified)
  size_t charset_len;
  const char *boundary;    // For multipart (NULL if not multipart)
  size_t boundary_len;
} mime_content_type_t;

/**
 * Parse Content-Type header value
 * Returns 0 on success, -1 on error
 */
int mime_parse_content_type(const char *content_type, size_t len,
                                       mime_content_type_t *result);

#ifdef __cplusplus
}
#endif

#endif /* MIME_UTILS_H */
