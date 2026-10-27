#include "mime_base64.h"

#include <stdint.h>
#include <string.h>

static const char mime_b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int mime_b64_digit(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
  if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
  if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

int mime_base64_encoded_size(size_t input_len, size_t *out_len) {
  size_t quartets;
  if (!out_len) return -1;
  quartets = input_len / 3u + (input_len % 3u != 0u);
  if (quartets > (SIZE_MAX - 1u) / 4u) return -1;
  *out_len = quartets * 4u;
  return 0;
}

int mime_base64_encode(const uint8_t *input, size_t input_len,
                       char *output, size_t capacity, size_t *written) {
  size_t length, i, pos = 0u;
  if (!output || !written || (!input && input_len != 0u) ||
      mime_base64_encoded_size(input_len, &length) != 0 ||
      capacity == 0u || length > capacity - 1u) return -1;
  for (i = 0u; i < input_len; ) {
    size_t remaining = input_len - i;
    uint32_t first = input[i++];
    uint32_t second = remaining > 1u ? input[i++] : 0u;
    uint32_t third = remaining > 2u ? input[i++] : 0u;
    output[pos++] = mime_b64_alphabet[(first >> 2) & 63u];
    output[pos++] = mime_b64_alphabet[((first & 3u) << 4) | (second >> 4)];
    output[pos++] = remaining > 1u
      ? mime_b64_alphabet[((second & 15u) << 2) | (third >> 6)] : '=';
    output[pos++] = remaining > 2u
      ? mime_b64_alphabet[third & 63u] : '=';
  }
  output[pos] = '\0';
  *written = pos;
  return 0;
}

static int mime_b64_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int mime_b64_validate(const char *input, size_t input_len,
                             size_t *decoded_size) {
  unsigned char digits[4];
  size_t n = 0u, size = 0u;
  int terminal = 0;
  for (size_t i = 0u; i < input_len; ++i) {
    const unsigned char ch = (unsigned char)input[i];
    int value;
    if (mime_b64_space(ch)) continue;
    if (terminal) return -1;
    value = ch == '=' ? 64 : mime_b64_digit(ch);
    if (value < 0) return -1;
    digits[n++] = (unsigned char)value;
    if (n == 4u) {
      size_t chunk;
      if (digits[0] == 64 || digits[1] == 64) return -1;
      if (digits[2] == 64) {
        if (digits[3] != 64 || (digits[1] & 15u) != 0u) return -1;
        chunk = 1u;
        terminal = 1;
      } else if (digits[3] == 64) {
        if ((digits[2] & 3u) != 0u) return -1;
        chunk = 2u;
        terminal = 1;
      } else {
        chunk = 3u;
      }
      if (size > SIZE_MAX - chunk) return -1;
      size += chunk;
      n = 0u;
    }
  }
  if (n != 0u) return -1;
  *decoded_size = size;
  return 0;
}

int mime_base64_decode(const char *input, size_t input_len,
                       uint8_t *output, size_t capacity, size_t *written) {
  size_t decoded_size, pos = 0u, n = 0u;
  unsigned char digits[4];
  if (!written || (!input && input_len != 0u)) return -1;
  if (mime_b64_validate(input, input_len, &decoded_size) != 0) return -1;
  if (!output) {
    if (capacity != 0u) return -1;
    *written = decoded_size;
    return 0;
  }
  if (capacity < decoded_size) return -1;

  /* Entire input was validated before touching caller output. */
  for (size_t i = 0u; i < input_len; ++i) {
    unsigned char ch = (unsigned char)input[i];
    if (mime_b64_space(ch)) continue;
    digits[n++] = (unsigned char)(ch == '=' ? 64 : mime_b64_digit(ch));
    if (n == 4u) {
      output[pos++] = (uint8_t)((digits[0] << 2) | (digits[1] >> 4));
      if (digits[2] != 64) {
        output[pos++] = (uint8_t)((digits[1] << 4) | (digits[2] >> 2));
        if (digits[3] != 64)
          output[pos++] = (uint8_t)((digits[2] << 6) | digits[3]);
      }
      n = 0u;
    }
  }
  *written = pos;
  return 0;
}
