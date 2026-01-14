#ifndef TURBONET_BASE64_UTILS_H
#define TURBONET_BASE64_UTILS_H

#include <platform.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encode binary data to Base64.
 * Allocates a null-terminated string in *output on success.
 * Returns 0 on success, -1 on error. */
CXX_C_API int tn_base64_encode(const uint8_t *data, size_t len, char **output);

/* Decode a Base64 string into a newly allocated buffer.
 * The binary output length is returned via *output_len.
 * Returns 0 on success, -1 on error. */
CXX_C_API int tn_base64_decode(const char *input, uint8_t **output, size_t *output_len);

#ifdef __cplusplus
}
#endif

#endif /* TURBONET_BASE64_UTILS_H */
