#include "base64_utils.h"

#include <libbase64.h>
#include <stdlib.h>
#include <string.h>

static tn_base64_error_t tn_base64_encoded_len(size_t len, size_t *out_len) {
  if (out_len == NULL) {
    return TN_BASE64_ERR_INVALID_ARG;
  }
  if (len > (SIZE_MAX - 2U) / 4U * 3U) {
    return TN_BASE64_ERR_OVERFLOW;
  }
  *out_len = 4U * ((len + 2U) / 3U);
  return TN_BASE64_OK;
}

tn_base64_string_result_t tn_base64_encode_ex(const uint8_t *data, size_t len) {
  tn_base64_string_result_t result = {.ok = false, .error = TN_BASE64_ERR_INVALID_ARG};
  size_t estimated = 0;
  size_t written = 0;

  if (data == NULL) {
    return result;
  }
  if (tn_base64_encoded_len(len, &estimated) != TN_BASE64_OK || estimated == SIZE_MAX) {
    result.error = TN_BASE64_ERR_OVERFLOW;
    return result;
  }

  /* Library does not add a terminator; allocate one extra byte. */
  char *buffer = malloc(estimated + 1);
  if (!buffer) {
    result.error = TN_BASE64_ERR_NO_MEMORY;
    return result;
  }

  base64_encode((const char *)data, len, buffer, &written, 0);
  buffer[written] = '\0';

  result.ok = true;
  result.value = buffer;
  return result;
}

int tn_base64_encode(const uint8_t *data, size_t len, char **output) {
  if (output == NULL) {
    return -1;
  }
  *output = NULL;
  tn_base64_string_result_t result = tn_base64_encode_ex(data, len);
  if (!result.ok) {
    return -1;
  }
  *output = result.value;
  return 0;
}

/**
 * @brief Encode binary data to Base64 into a caller-supplied buffer.
 *
 * No heap allocation.  Returns 0 on success, -1 if @p out_cap is too small
 * (need 4*ceil(len/3) + 1 bytes, i.e. ceil_div(len,3)*4 + 1).
 */
tn_base64_error_t tn_base64_encode_buf_ex(const uint8_t *data, size_t len,
                                          char *out, size_t out_cap) {
  size_t needed;

  if (!data || !out || out_cap == 0)
    return TN_BASE64_ERR_INVALID_ARG;

  if (tn_base64_encoded_len(len, &needed) != TN_BASE64_OK || needed == SIZE_MAX) {
    return TN_BASE64_ERR_OVERFLOW;
  }
  needed += 1; /* +1 for NUL */
  if (out_cap < needed)
    return TN_BASE64_ERR_BUFFER_TOO_SMALL;

  size_t written = 0;
  base64_encode((const char *)data, len, out, &written, 0);
  out[written] = '\0';
  return TN_BASE64_OK;
}

int tn_base64_encode_buf(const uint8_t *data, size_t len,
                         char *out, size_t out_cap) {
  return tn_base64_encode_buf_ex(data, len, out, out_cap) == TN_BASE64_OK ? 0 : -1;
}

tn_base64_bytes_result_t tn_base64_decode_ex(const char *input) {
  tn_base64_bytes_result_t result = {.ok = false, .error = TN_BASE64_ERR_INVALID_ARG};
  size_t in_len;
  size_t estimated;
  uint8_t *buffer;
  size_t written = 0;

  if (input == NULL) {
    return result;
  }

  in_len = strlen(input);
  if (in_len == 0) {
    return result;
  }
  if (in_len > ((SIZE_MAX - 3U) / 3U) * 4U) {
    result.error = TN_BASE64_ERR_OVERFLOW;
    return result;
  }

  /* 3/4 of input size is safe upper bound for decoded bytes. */
  estimated = (in_len / 4U) * 3U + 3U;
  buffer = malloc(estimated);
  if (!buffer) {
    result.error = TN_BASE64_ERR_NO_MEMORY;
    return result;
  }

  /* base64_decode returns 1 on success, 0 on invalid input. */
  int rc = base64_decode(input, in_len, (char *)buffer, &written, 0);
  if (rc != 1) {
    free(buffer);
    result.error = TN_BASE64_ERR_INVALID_INPUT;
    return result;
  }

  result.ok = true;
  result.value.data = buffer;
  result.value.len = written;
  return result;
}

int tn_base64_decode(const char *input, uint8_t **output, size_t *output_len) {
  if (!output || !output_len) {
    return -1;
  }
  *output = NULL;
  *output_len = 0;

  tn_base64_bytes_result_t result = tn_base64_decode_ex(input);
  if (!result.ok) {
    return -1;
  }

  *output = result.value.data;
  *output_len = result.value.len;
  return 0;
}
