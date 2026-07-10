#ifndef turboutils_BASE64_UTILS_H
#define turboutils_BASE64_UTILS_H

#include <platform.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TN_BASE64_OK = 0,
  TN_BASE64_ERR_INVALID_ARG = -1,
  TN_BASE64_ERR_NO_MEMORY = -2,
  TN_BASE64_ERR_BUFFER_TOO_SMALL = -3,
  TN_BASE64_ERR_INVALID_INPUT = -4,
  TN_BASE64_ERR_OVERFLOW = -5
} tn_base64_error_t;

typedef struct {
  bool ok;
  union {
    char *value;
    tn_base64_error_t error;
  };
} tn_base64_string_result_t;

typedef struct {
  uint8_t *data;
  size_t len;
} tn_base64_bytes_t;

typedef struct {
  bool ok;
  union {
    tn_base64_bytes_t value;
    tn_base64_error_t error;
  };
} tn_base64_bytes_result_t;

/* Result-style APIs. Caller owns returned value on success and frees it with free(). */
CXX_C_API tn_base64_string_result_t tn_base64_encode_ex(const uint8_t *data, size_t len);
CXX_C_API tn_base64_error_t tn_base64_encode_buf_ex(const uint8_t *data, size_t len,
                                                    char *out, size_t out_cap);
CXX_C_API tn_base64_bytes_result_t tn_base64_decode_ex(const char *input);

/* Encode binary data to Base64.
 * Allocates a null-terminated string in *output on success.
 * Returns 0 on success, -1 on error. */
CXX_C_API int tn_base64_encode(const uint8_t *data, size_t len, char **output);

/* Encode binary data to Base64 into a caller-supplied buffer (no malloc).
 * out_cap must be >= 4*ceil(len/3)+1.  Returns 0 on success, -1 on error. */
CXX_C_API int tn_base64_encode_buf(const uint8_t *data, size_t len,
                                   char *out, size_t out_cap);

/* Decode a Base64 string into a newly allocated buffer.
 * The binary output length is returned via *output_len.
 * Returns 0 on success, -1 on error. */
CXX_C_API int tn_base64_decode(const char *input, uint8_t **output, size_t *output_len);

#ifdef __cplusplus
}
#endif

#endif /* turboutils_BASE64_UTILS_H */
