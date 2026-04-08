#include "mime_encoded_word.h"
#include "mime_utils.h"
#include "turbo_str.h"
#include <string.h>
#include <ctype.h>

/* ── Detection ─────────────────────────────────────────────────────── */

int mime_is_encoded_word(const char *str, size_t len) {
  return len >= 2 && str[0] == '=' && str[1] == '?';
}

/* ── Parsing ───────────────────────────────────────────────────────── */

size_t mime_parse_encoded_word(const char *str, size_t len,
                                mime_encoded_word_t *result) {
  if (!str || !result || len < 7) return 0; // Minimum: =?x?x?x?=

  if (!mime_is_encoded_word(str, len)) return 0;

  memset(result, 0, sizeof(*result));

  const char *ptr = str + 2; // Skip "=?"
  const char *end = str + len;

  // Parse charset
  result->charset = ptr;
  while (ptr < end && *ptr != '?') ptr++;
  if (ptr >= end) return 0;
  result->charset_len = ptr - result->charset;
  ptr++; // Skip '?'

  // Parse encoding
  if (ptr >= end) return 0;
  char enc = *ptr;
  if (enc == 'B' || enc == 'b') {
    result->encoding = MIME_EW_ENCODING_BASE64;
  } else if (enc == 'Q' || enc == 'q') {
    result->encoding = MIME_EW_ENCODING_QUOTED_PRINTABLE;
  } else {
    return 0;
  }
  ptr++; // Skip encoding char

  if (ptr >= end || *ptr != '?') return 0;
  ptr++; // Skip '?'

  // Parse encoded text
  result->encoded_text = ptr;
  while (ptr < end - 1) {
    if (ptr[0] == '?' && ptr[1] == '=') {
      result->encoded_text_len = ptr - result->encoded_text;
      return (ptr + 2) - str; // Total length including ?=
    }
    ptr++;
  }

  return 0; // No closing ?=
}

/* ── Decoding ──────────────────────────────────────────────────────── */

static char *decode_qp_encoded_word(mem_pool_t *pool, const char *input, size_t len) {
  // RFC 2047 Q encoding: like quoted-printable but _ = space
  char *temp = mem_alloc(pool, len + 1);
  if (!temp) return NULL;

  size_t out_pos = 0;
  for (size_t i = 0; i < len; i++) {
    if (input[i] == '_') {
      temp[out_pos++] = ' ';
    } else if (input[i] == '=' && i + 2 < len) {
      // Hex decode
      int high = (input[i+1] >= '0' && input[i+1] <= '9') ? input[i+1] - '0' :
                 (input[i+1] >= 'A' && input[i+1] <= 'F') ? input[i+1] - 'A' + 10 :
                 (input[i+1] >= 'a' && input[i+1] <= 'f') ? input[i+1] - 'a' + 10 : -1;
      int low = (input[i+2] >= '0' && input[i+2] <= '9') ? input[i+2] - '0' :
                (input[i+2] >= 'A' && input[i+2] <= 'F') ? input[i+2] - 'A' + 10 :
                (input[i+2] >= 'a' && input[i+2] <= 'f') ? input[i+2] - 'a' + 10 : -1;
      if (high >= 0 && low >= 0) {
        temp[out_pos++] = (char)((high << 4) | low);
        i += 2;
      } else {
        temp[out_pos++] = input[i];
      }
    } else {
      temp[out_pos++] = input[i];
    }
  }
  temp[out_pos] = '\0';
  return temp;
}

char *mime_decode_encoded_word(mem_pool_t *pool, const mime_encoded_word_t *ew) {
  if (!pool || !ew) return NULL;

  if (ew->encoding == MIME_EW_ENCODING_BASE64) {
    // tn_base64_decode needs null-terminated string, copy first
    char *encoded_copy = mem_alloc(pool, ew->encoded_text_len + 1);
    if (!encoded_copy) return NULL;
    memcpy(encoded_copy, ew->encoded_text, ew->encoded_text_len);
    encoded_copy[ew->encoded_text_len] = '\0';

    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    if (mime_decode_base64(pool, encoded_copy, ew->encoded_text_len,
                           &decoded, &decoded_len) != 0) {
      return NULL;
    }
    // Ensure null termination
    char *result = mem_alloc(pool, decoded_len + 1);
    if (!result) return NULL;
    memcpy(result, decoded, decoded_len);
    result[decoded_len] = '\0';
    return result;
  } else if (ew->encoding == MIME_EW_ENCODING_QUOTED_PRINTABLE) {
    return decode_qp_encoded_word(pool, ew->encoded_text, ew->encoded_text_len);
  }

  return NULL;
}

/* ── Header decoding ───────────────────────────────────────────────── */

char *mime_decode_header(mem_pool_t *pool, const char *header_value, size_t len) {
  if (!pool || !header_value) return NULL;

  // Allocate output buffer (worst case: same size as input)
  char *output = mem_alloc(pool, len * 2 + 1);
  if (!output) return NULL;

  size_t out_pos = 0;
  size_t i = 0;

  while (i < len) {
    // Check for encoded-word
    if (mime_is_encoded_word(header_value + i, len - i)) {
      mime_encoded_word_t ew;
      size_t ew_len = mime_parse_encoded_word(header_value + i, len - i, &ew);

      if (ew_len > 0) {
        // Decode encoded-word
        char *decoded = mime_decode_encoded_word(pool, &ew);
        if (decoded) {
          size_t decoded_len = strlen(decoded);
          memcpy(output + out_pos, decoded, decoded_len);
          out_pos += decoded_len;
        }
        i += ew_len;

        // Skip whitespace between adjacent encoded-words (RFC 2047)
        while (i < len && (header_value[i] == ' ' || header_value[i] == '\t')) {
          if (i + 1 < len && mime_is_encoded_word(header_value + i + 1, len - i - 1)) {
            i++; // Skip whitespace
          } else {
            break;
          }
        }
        continue;
      }
    }

    // Copy plain text
    output[out_pos++] = header_value[i++];
  }

  output[out_pos] = '\0';
  return output;
}

char *mime_decode_header_auto(mem_pool_t *pool, const char *header_value) {
  if (!header_value) return NULL;
  return mime_decode_header(pool, header_value, strlen(header_value));
}
