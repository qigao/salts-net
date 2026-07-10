#include "mime_rfc2231.h"
#include "turbo_simd_scan.h"
#include "turbo_str.h"
#include <string.h>
#include <ctype.h>

/* ── Detection ─────────────────────────────────────────────────────── */

int mime_is_rfc2231_param(const char *param_name, size_t len) {
  if (!param_name || len == 0) return 0;

  // Check if ends with *
  if (param_name[len - 1] == '*') return 1;

  // Check for *N* pattern (continuation)
  for (size_t i = 0; i < len; i++) {
    if (param_name[i] == '*' && i + 1 < len && isdigit(param_name[i + 1])) {
      return 1;
    }
  }

  return 0;
}

/* ── Parsing ───────────────────────────────────────────────────────── */

int mime_parse_rfc2231_value(const char *value, size_t len,
                              mime_rfc2231_param_t *result) {
  if (!value || !result) return -1;

  memset(result, 0, sizeof(*result));
  result->is_encoded = 1;

  const char *ptr = value;
  const char *end = value + len;

  // Parse charset (before first ')
  result->charset = ptr;
  ptr = turbo_scan_to_char(ptr, end, '\'');
  if (ptr >= end) return -1;
  result->charset_len = ptr - result->charset;
  ptr++; // Skip '

  // Parse language (before second ')
  result->language = ptr;
  ptr = turbo_scan_to_char(ptr, end, '\'');
  if (ptr >= end) return -1;
  result->language_len = ptr - result->language;
  ptr++; // Skip '

  // Rest is encoded value
  result->value = ptr;
  result->value_len = end - ptr;

  return 0;
}

/* ── Percent decoding ──────────────────────────────────────────────── */

static int hex_to_int(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

char *mime_decode_rfc2231_value(mem_pool_t *pool,
                                 const mime_rfc2231_param_t *param) {
  if (!pool || !param || !param->value) return NULL;

  // Allocate output buffer (decoded will be <= original)
  char *output = mem_alloc(pool, param->value_len + 1);
  if (!output) return NULL;

  size_t out_pos = 0;
  for (size_t i = 0; i < param->value_len; i++) {
    if (param->value[i] == '%' && i + 2 < param->value_len) {
      int high = hex_to_int(param->value[i + 1]);
      int low = hex_to_int(param->value[i + 2]);
      if (high >= 0 && low >= 0) {
        output[out_pos++] = (char)((high << 4) | low);
        i += 2;
        continue;
      }
    }
    output[out_pos++] = param->value[i];
  }

  output[out_pos] = '\0';
  return output;
}

char *mime_decode_rfc2231(mem_pool_t *pool, const char *value, size_t len) {
  if (!pool || !value) return NULL;

  mime_rfc2231_param_t param;
  if (mime_parse_rfc2231_value(value, len, &param) != 0) {
    return NULL;
  }

  return mime_decode_rfc2231_value(pool, &param);
}

/* ── Parameter name helpers ────────────────────────────────────────── */

size_t mime_rfc2231_base_name(const char *param_name, size_t len,
                               char *output, size_t output_size) {
  if (!param_name || !output || output_size == 0) return 0;

  // Find first * or end
  size_t base_len = 0;
  {
    const char *star = turbo_scan_char(param_name, param_name + len, '*');
    base_len = star ? (size_t)(star - param_name) : len;
  }

  if (base_len >= output_size) {
    base_len = output_size - 1;
  }

  memcpy(output, param_name, base_len);
  output[base_len] = '\0';
  return base_len;
}

int mime_rfc2231_continuation_index(const char *param_name, size_t len) {
  if (!param_name || len == 0) return -1;

  // Look for *N* or *N pattern
  for (size_t i = 0; i < len; i++) {
    if (param_name[i] == '*' && i + 1 < len && isdigit(param_name[i + 1])) {
      return param_name[i + 1] - '0';
    }
  }

  return -1;
}

/* ── Content-Disposition helper ────────────────────────────────────── */

static const char *find_param_rfc2231(const char *start, const char *end,
                                       const char *param_name, size_t param_len,
                                       size_t *value_len) {
  const char *ptr = start;

  while (ptr < end) {
    // Skip whitespace
    for (;;) {
      ptr = turbo_scan_skip_sp_tab(ptr, end);
      if (ptr < end && *ptr == ';') {
        ptr++;
        continue;
      }
      break;
    }
    if (ptr >= end) break;

    // Check parameter name
    const char *name_start = ptr;
    ptr = turbo_scan_to_any3(ptr, end, '=', ';', ' ');

    size_t name_len = ptr - name_start;

    // Check if this is our parameter (with or without *)
    int match = 0;
    if (name_len == param_len && memcmp(name_start, param_name, param_len) == 0) {
      match = 1;
    } else if (name_len == param_len + 1 &&
               memcmp(name_start, param_name, param_len) == 0 &&
               name_start[param_len] == '*') {
      match = 1;
    }

    if (match) {
      // Skip whitespace and =
      ptr = turbo_scan_skip_sp_tab(ptr, end);
      if (ptr >= end || *ptr != '=') continue;
      ptr++; // Skip =
      ptr = turbo_scan_skip_sp_tab(ptr, end);

      // Parse value
      const char *value_start = ptr;
      if (ptr < end && *ptr == '"') {
        // Quoted value
        ptr++;
        value_start = ptr;
        while (ptr < end) {
          ptr = turbo_scan_to_any2(ptr, end, '"', '\\');
          if (ptr >= end || *ptr == '"') break;
          if (*ptr == '\\' && ptr + 1 < end) ptr += 2;
          else ptr++;
        }
        *value_len = ptr - value_start;
        return value_start;
      } else {
        // Unquoted value
        ptr = turbo_scan_to_char(ptr, end, ';');
        *value_len = ptr - value_start;
        // Trim trailing whitespace
        while (*value_len > 0 && (value_start[*value_len - 1] == ' ' ||
                                   value_start[*value_len - 1] == '\t')) {
          (*value_len)--;
        }
        return value_start;
      }
    }

    // Skip to next parameter
    ptr = turbo_scan_to_char(ptr, end, ';');
  }

  return NULL;
}

char *mime_get_filename_rfc2231(mem_pool_t *pool,
                                 const char *content_disposition,
                                 size_t len) {
  if (!pool || !content_disposition) return NULL;

  const char *ptr = content_disposition;
  const char *end = content_disposition + len;

  // Skip disposition type
  ptr = turbo_scan_to_char(ptr, end, ';');

  // Try filename* first (RFC 2231 encoded)
  size_t value_len = 0;
  const char *value = find_param_rfc2231(ptr, end, "filename", 8, &value_len);

  if (value && value_len > 0) {
    // Check if it's RFC 2231 encoded (contains charset'language')
    const char *quote_pos = turbo_scan_char(value, value + value_len, '\'');
    if (quote_pos) {
      // RFC 2231 encoded
      return mime_decode_rfc2231(pool, value, value_len);
    } else {
      // Plain filename
      char *result = mem_alloc(pool, value_len + 1);
      if (!result) return NULL;
      memcpy(result, value, value_len);
      result[value_len] = '\0';
      return result;
    }
  }

  return NULL;
}
