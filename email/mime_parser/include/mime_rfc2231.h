/**
 * @file mime_rfc2231.h
 * @brief RFC 2231 MIME Parameter Value Encoding
 *
 * Handles encoded parameter values in MIME headers, commonly used for
 * non-ASCII filenames in Content-Disposition headers.
 *
 * Examples:
 *   filename*=utf-8''%E6%B5%8B%E8%AF%95.txt
 *   filename*0*=utf-8''%E6%B5%8B;
 *   filename*1*=%E8%AF%95.txt
 *
 * This allows filenames with Unicode characters to be properly encoded
 * in email attachments and HTTP downloads.
 */

#ifndef MIME_RFC2231_H
#define MIME_RFC2231_H

#include "platform.h"
#include <stddef.h>
#include "salts_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── RFC 2231 Parameter ────────────────────────────────────────────── */

typedef struct {
  const char *charset;      // e.g., "utf-8", "iso-8859-1"
  size_t charset_len;
  const char *language;     // e.g., "en", "zh" (usually empty)
  size_t language_len;
  const char *value;        // Percent-encoded value
  size_t value_len;
  int is_encoded;           // 1 if encoded (has *), 0 if plain
} mime_rfc2231_param_t;

/* ── API ───────────────────────────────────────────────────────────── */

/**
 * Check if parameter name indicates RFC 2231 encoding
 * Returns 1 if name ends with * or *N* (where N is digit)
 */
int mime_is_rfc2231_param(const char *param_name, size_t len);

/**
 * Parse RFC 2231 encoded parameter value
 *
 * Input formats:
 *   charset'language'encoded-value
 *   charset''encoded-value
 *
 * Example:
 *   utf-8''%E6%B5%8B%E8%AF%95.txt
 *
 * Returns 0 on success, -1 on error
 */
int mime_parse_rfc2231_value(const char *value, size_t len,
                                        mime_rfc2231_param_t *result);

/**
 * Decode RFC 2231 percent-encoded value
 * Allocates decoded string using pool
 * Returns NULL on error
 */
char *mime_decode_rfc2231_value(mem_pool_t *pool,
                                           const mime_rfc2231_param_t *param);

/**
 * Decode RFC 2231 parameter directly from value string
 * Convenience function combining parse + decode
 * Returns NULL on error
 */
char *mime_decode_rfc2231(mem_pool_t *pool,
                                     const char *value, size_t len);

/**
 * Extract parameter name without RFC 2231 suffix
 * Example: "filename*0*" -> "filename"
 * Writes to output buffer, returns length
 */
size_t mime_rfc2231_base_name(const char *param_name, size_t len,
                                         char *output, size_t output_size);

/**
 * Get continuation index from parameter name
 * Example: "filename*0*" -> 0, "filename*1*" -> 1
 * Returns -1 if not a continuation parameter
 */
int mime_rfc2231_continuation_index(const char *param_name, size_t len);

/* ── Helper for Content-Disposition ────────────────────────────────── */

/**
 * Enhanced filename extraction with RFC 2231 support
 * Tries filename* first, falls back to filename
 * Returns pool-allocated string or NULL
 */
char *mime_get_filename_rfc2231(mem_pool_t *pool,
                                           const char *content_disposition,
                                           size_t len);

/* ── Encoding (send-side) ───────────────────────────────────────────── */

/**
 * Encode a filename for use in Content-Disposition using RFC 2231.
 *
 * Returns a pool-allocated string of the form "UTF-8''%XX%XX..."
 * when the filename contains non-ASCII characters; returns NULL when
 * the filename is pure ASCII (caller should fall back to filename="...").
 *
 * Only characters that are safe RFC 2231 attr-chars are left unencoded;
 * all others (including space, quotes, and bytes > 0x7F) are percent-encoded.
 */
char *mime_encode_rfc2231_filename(mem_pool_t *pool,
                                              const char *filename, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MIME_RFC2231_H */
