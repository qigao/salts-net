/* Native RFC 4648 Base64 byte codec for MIME and protocol authorization.
 * No NUL-dependent input, hidden heap allocation or legacy provider shim.
 * Caller owns both input and output buffers. Only this module implements the
 * codec; MIME, SMTP and HTTP proxy all use it.
 */
#ifndef SALTSNET_MIME_BASE64_H
#define SALTSNET_MIME_BASE64_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Required output bytes excluding the trailing NUL. Reject size overflow. */
int mime_base64_encoded_size(size_t input_len, size_t *out_len);

/* Emits exactly out_len characters and a NUL terminator. Output and written
 * are unchanged on failure. input may be NULL only for input_len=0. */
int mime_base64_encode(const uint8_t *input, size_t input_len,
                       char *output, size_t capacity, size_t *written);

/* Strict canonical Base64: RFC2045 SP/HT/CR/LF folding accepted; padding is
 * required, only final quartet may contain '=', and discarded bits must be
 * zero. output=NULL/capacity=0 queries required decoded bytes after FULL
 * syntax validation; output is unchanged on any failure. The decoded data
 * need not be terminated and may contain embedded NUL. */
int mime_base64_decode(const char *input, size_t input_len,
                       uint8_t *output, size_t capacity, size_t *written);

#ifdef __cplusplus
}
#endif
#endif
