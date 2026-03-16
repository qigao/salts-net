#include "mime_utils.h"
#include "base64_utils.h"
#include "turbo_str.h"
#include "turbo_buffer.h"
#include <string.h>
#include <stdlib.h>

/* ── Encoding detection ────────────────────────────────────────────── */

mime_encoding_t mime_parse_encoding(const char *encoding_str, size_t len) {
  if (!encoding_str || len == 0) return MIME_ENCODING_7BIT;

  // Normalize: lowercase comparison
  if (len == 4 && tstr_ncasecmp(encoding_str, "7bit", 4) == 0) {
    return MIME_ENCODING_7BIT;
  }
  if (len == 4 && tstr_ncasecmp(encoding_str, "8bit", 4) == 0) {
    return MIME_ENCODING_8BIT;
  }
  if (len == 6 && tstr_ncasecmp(encoding_str, "binary", 6) == 0) {
    return MIME_ENCODING_BINARY;
  }
  if (len == 6 && tstr_ncasecmp(encoding_str, "base64", 6) == 0) {
    return MIME_ENCODING_BASE64;
  }
  if (len >= 16 && tstr_ncasecmp(encoding_str, "quoted-printable", 16) == 0) {
    return MIME_ENCODING_QUOTED_PRINTABLE;
  }

  return MIME_ENCODING_UNKNOWN;
}

const char *mime_encoding_name(mime_encoding_t encoding) {
  switch (encoding) {
    case MIME_ENCODING_7BIT: return "7bit";
    case MIME_ENCODING_8BIT: return "8bit";
    case MIME_ENCODING_BINARY: return "binary";
    case MIME_ENCODING_BASE64: return "base64";
    case MIME_ENCODING_QUOTED_PRINTABLE: return "quoted-printable";
    default: return "unknown";
  }
}

/* ── Quoted-Printable decoder ──────────────────────────────────────── */

static int hex_to_int(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

int mime_decode_quoted_printable(mem_pool_t *pool,
                                  const char *input, size_t input_len,
                                  char **output, size_t *output_len) {
  if (!pool || !input || !output || !output_len) return -1;

  // Worst case: output same size as input
  char *result = mem_alloc(pool, input_len + 1);
  if (!result) return -1;

  size_t out_pos = 0;
  size_t i = 0;

  while (i < input_len) {
    if (input[i] == '=') {
      if (i + 2 < input_len) {
        // Soft line break: =\r\n or =\n
        if (input[i + 1] == '\r' && input[i + 2] == '\n') {
          i += 3;
          continue;
        }
        if (input[i + 1] == '\n') {
          i += 2;
          continue;
        }

        // Hex encoded character: =XX
        int high = hex_to_int(input[i + 1]);
        int low = hex_to_int(input[i + 2]);
        if (high >= 0 && low >= 0) {
          result[out_pos++] = (char)((high << 4) | low);
          i += 3;
          continue;
        }
      }
      // Invalid sequence, copy as-is
      result[out_pos++] = input[i++];
    } else {
      result[out_pos++] = input[i++];
    }
  }

  result[out_pos] = '\0';
  *output = result;
  *output_len = out_pos;
  return 0;
}

/* ── Base64 decoder (wrapper) ──────────────────────────────────────── */

int mime_decode_base64(mem_pool_t *pool,
                       const char *input, size_t input_len,
                       uint8_t **output, size_t *output_len) {
  if (!pool || !input || !output || !output_len) return -1;

  // Use base64_utils (allocates with malloc)
  uint8_t *decoded = NULL;
  size_t decoded_len = 0;

  if (tn_base64_decode(input, &decoded, &decoded_len) != 0) {
    return -1;
  }

  // Copy to pool memory
  uint8_t *result = mem_alloc(pool, decoded_len);
  if (!result) {
    free(decoded);
    return -1;
  }

  memcpy(result, decoded, decoded_len);
  free(decoded);

  *output = result;
  *output_len = decoded_len;
  return 0;
}

/* ── Unified decoder ───────────────────────────────────────────────── */

int mime_decode_body(mem_pool_t *pool,
                     const char *input, size_t input_len,
                     mime_encoding_t encoding,
                     char **output, size_t *output_len) {
  if (!pool || !input || !output || !output_len) return -1;

  switch (encoding) {
    case MIME_ENCODING_7BIT:
    case MIME_ENCODING_8BIT:
    case MIME_ENCODING_BINARY:
      // No decoding needed, copy as-is
      *output = mem_alloc(pool, input_len + 1);
      if (!*output) return -1;
      memcpy(*output, input, input_len);
      (*output)[input_len] = '\0';
      *output_len = input_len;
      return 0;

    case MIME_ENCODING_BASE64:
      return mime_decode_base64(pool, input, input_len, (uint8_t **)output, output_len);

    case MIME_ENCODING_QUOTED_PRINTABLE:
      return mime_decode_quoted_printable(pool, input, input_len, output, output_len);

    default:
      return -1;
  }
}

/* ── Content-Type parser ───────────────────────────────────────────── */

static void skip_whitespace(const char **ptr, const char *end) {
  while (*ptr < end && (**ptr == ' ' || **ptr == '\t')) {
    (*ptr)++;
  }
}

static const char *find_param(const char *start, const char *end,
                               const char *param_name, size_t param_len,
                               size_t *value_len) {
  const char *ptr = start;

  while (ptr < end) {
    // Find parameter name
    skip_whitespace(&ptr, end);
    if (ptr >= end) break;

    // Check if matches param_name
    if (ptr + param_len <= end && tstr_ncasecmp(ptr, param_name, param_len) == 0) {
      ptr += param_len;
      skip_whitespace(&ptr, end);

      if (ptr < end && *ptr == '=') {
        ptr++;
        skip_whitespace(&ptr, end);

        // Parse value (quoted or unquoted)
        const char *value_start = ptr;
        if (ptr < end && *ptr == '"') {
          // Quoted value
          ptr++;
          value_start = ptr;
          while (ptr < end && *ptr != '"') ptr++;
          *value_len = ptr - value_start;
          return value_start;
        } else {
          // Unquoted value (until semicolon or end)
          while (ptr < end && *ptr != ';') ptr++;
          *value_len = ptr - value_start;
          // Trim trailing whitespace
          while (*value_len > 0 && (value_start[*value_len - 1] == ' ' ||
                                     value_start[*value_len - 1] == '\t')) {
            (*value_len)--;
          }
          return value_start;
        }
      }
    }

    // Skip to next parameter
    while (ptr < end && *ptr != ';') ptr++;
    if (ptr < end) ptr++; // Skip semicolon
  }

  return NULL;
}

int mime_parse_content_type(const char *content_type, size_t len,
                             mime_content_type_t *result) {
  if (!content_type || !result) return -1;

  memset(result, 0, sizeof(*result));

  const char *ptr = content_type;
  const char *end = content_type + len;

  // Parse type/subtype
  const char *type_start = ptr;
  while (ptr < end && *ptr != '/' && *ptr != ';') ptr++;

  if (ptr >= end || *ptr != '/') return -1;

  result->type = type_start;
  result->type_len = ptr - type_start;
  ptr++; // Skip '/'

  const char *subtype_start = ptr;
  while (ptr < end && *ptr != ';' && *ptr != ' ') ptr++;

  result->subtype = subtype_start;
  result->subtype_len = ptr - subtype_start;

  // Parse parameters
  result->charset = find_param(ptr, end, "charset", 7, &result->charset_len);
  result->boundary = find_param(ptr, end, "boundary", 8, &result->boundary_len);

  return 0;
}
