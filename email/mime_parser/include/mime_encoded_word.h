/**
 * @file mime_encoded_word.h
 * @brief RFC 2047 MIME Encoded-Word decoder
 *
 * Decodes encoded-words in email headers:
 *   =?charset?encoding?encoded-text?=
 *
 * Examples:
 *   =?UTF-8?B?SGVsbG8gV29ybGQ=?=           -> "Hello World"
 *   =?ISO-8859-1?Q?Fran=E7ois?=            -> "François"
 *   =?UTF-8?B?5L2g5aW9?=                   -> "你好"
 *
 * Used in email headers like Subject, From, To, etc.
 */

#ifndef MIME_ENCODED_WORD_H
#define MIME_ENCODED_WORD_H

#include "platform.h"
#include <stddef.h>
#include "salts_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Encoded-word structure ────────────────────────────────────────── */

typedef enum {
  MIME_EW_ENCODING_BASE64,           // 'B' or 'b'
  MIME_EW_ENCODING_QUOTED_PRINTABLE, // 'Q' or 'q'
  MIME_EW_ENCODING_UNKNOWN
} mime_ew_encoding_t;

typedef struct {
  const char *charset;      // e.g., "UTF-8", "ISO-8859-1"
  size_t charset_len;
  mime_ew_encoding_t encoding;
  const char *encoded_text; // The encoded portion
  size_t encoded_text_len;
} mime_encoded_word_t;

/* ── API ───────────────────────────────────────────────────────────── */

/**
 * Check if string starts with encoded-word pattern: =?
 * Returns 1 if yes, 0 if no
 */
int mime_is_encoded_word(const char *str, size_t len);

/**
 * Parse single encoded-word
 * Returns length of parsed encoded-word, or 0 if invalid
 */
size_t mime_parse_encoded_word(const char *str, size_t len,
                                          mime_encoded_word_t *result);

/**
 * Decode single encoded-word to pool-allocated string
 * Returns decoded string or NULL on error
 */
char *mime_decode_encoded_word(mem_pool_t *pool,
                                          const mime_encoded_word_t *ew);

/**
 * Decode entire header value containing multiple encoded-words
 * Handles mixed encoded and plain text
 *
 * Example:
 *   "Hello =?UTF-8?B?V29ybGQ=?= from =?UTF-8?Q?John?="
 *   -> "Hello World from John"
 *
 * Returns decoded string or NULL on error
 */
char *mime_decode_header(mem_pool_t *pool,
                                    const char *header_value,
                                    size_t len);

/**
 * Decode header with automatic buffer sizing
 * Convenience wrapper around mime_decode_header
 */
char *mime_decode_header_auto(mem_pool_t *pool,
                                         const char *header_value);

/* ── Encoding functions (send-side) ────────────────────────────────── */

/**
 * Encode a header value as RFC 2047 encoded-word(s) when it contains
 * non-ASCII characters; returns a pool-allocated copy of the plain value
 * when the input is pure ASCII (zero overhead on the fast path).
 *
 * The output is one or more space-separated =?UTF-8?B?...?= words, each
 * within the 75-character encoded-word length limit (RFC 2047 §2).
 */
char *mime_encode_header_if_needed(mem_pool_t *pool,
                                              const char *value, size_t len);

/**
 * Wrap a raw base64 string with RFC 2045 §6.8 line folding:
 * inserts "\r\n" after every 76 characters.
 * Returns a pool-allocated string; the trailing "\r\n" is NOT appended.
 */
char *mime_base64_fold(mem_pool_t *pool,
                                  const char *b64, size_t b64_len);

#ifdef __cplusplus
}
#endif

#endif /* MIME_ENCODED_WORD_H */
