#include "mime_content_disposition.h"
#include "turbo_simd_scan.h"
#include "turbo_str.h"
#include <string.h>

/* ── Type name mapping ─────────────────────────────────────────────── */

const char *mime_disposition_type_name(mime_disposition_type_t type) {
  switch (type) {
    case MIME_DISPOSITION_INLINE: return "inline";
    case MIME_DISPOSITION_ATTACHMENT: return "attachment";
    case MIME_DISPOSITION_FORM_DATA: return "form-data";
    default: return "unknown";
  }
}

/* ── Parsing helpers ───────────────────────────────────────────────── */

static void skip_whitespace(const char **ptr, const char *end) {
  *ptr = turbo_scan_skip_sp_tab(*ptr, end);
}

static mime_disposition_type_t parse_disposition_type(const char *type_str, size_t len) {
  if (len == 6 && tstr_ncasecmp(type_str, "inline", 6) == 0) {
    return MIME_DISPOSITION_INLINE;
  }
  if (len == 10 && tstr_ncasecmp(type_str, "attachment", 10) == 0) {
    return MIME_DISPOSITION_ATTACHMENT;
  }
  if (len == 9 && tstr_ncasecmp(type_str, "form-data", 9) == 0) {
    return MIME_DISPOSITION_FORM_DATA;
  }
  return MIME_DISPOSITION_UNKNOWN;
}

static const char *find_param_value(const char *start, const char *end,
                                     const char *param_name, size_t param_len,
                                     size_t *value_len) {
  const char *ptr = start;

  while (ptr < end) {
    skip_whitespace(&ptr, end);
    if (ptr >= end) break;

    // Check if matches param_name
    const char *name_start = ptr;
    ptr = turbo_scan_to_any3(ptr, end, '=', ';', ' ');

    size_t name_len = ptr - name_start;

    if (name_len == param_len && tstr_ncasecmp(name_start, param_name, param_len) == 0) {
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
          while (ptr < end) {
            ptr = turbo_scan_to_any2(ptr, end, '"', '\\');
            if (ptr >= end || *ptr == '"') {
              break;
            }
            if (*ptr == '\\' && ptr + 1 < end) ptr += 2;
            else ptr++;
          }
          *value_len = ptr - value_start;
          return value_start;
        } else {
          // Unquoted value (until semicolon or end)
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
    }

    // Skip to next parameter
    ptr = turbo_scan_to_char(ptr, end, ';');
    if (ptr < end) ptr++; // Skip semicolon
  }

  return NULL;
}

static size_t parse_size_param(const char *start, const char *end) {
  size_t size_len = 0;
  const char *size_str = find_param_value(start, end, "size", 4, &size_len);

  if (!size_str) return 0;

  size_t result = 0;
  for (size_t i = 0; i < size_len; i++) {
    if (size_str[i] >= '0' && size_str[i] <= '9') {
      result = result * 10 + (size_str[i] - '0');
    } else {
      break;
    }
  }

  return result;
}

/* ── Main parser ───────────────────────────────────────────────────── */

int mime_parse_content_disposition(const char *value, size_t len,
                                    mime_content_disposition_t *result) {
  if (!value || !result) return -1;

  memset(result, 0, sizeof(*result));

  const char *ptr = value;
  const char *end = value + len;

  // Parse disposition type
  const char *type_start = ptr;
  ptr = turbo_scan_to_any2(ptr, end, ';', ' ');

  size_t type_len = ptr - type_start;
  result->type = parse_disposition_type(type_start, type_len);

  // Parse parameters
  result->filename = find_param_value(ptr, end, "filename", 8, &result->filename_len);
  result->name = find_param_value(ptr, end, "name", 4, &result->name_len);
  result->creation_date = find_param_value(ptr, end, "creation-date", 13, &result->creation_date_len);
  result->modification_date = find_param_value(ptr, end, "modification-date", 17, &result->modification_date_len);
  result->size = parse_size_param(ptr, end);

  return 0;
}

/* ── Helper functions ──────────────────────────────────────────────── */

char *mime_disposition_get_filename(mem_pool_t *pool,
                                     const mime_content_disposition_t *disp) {
  if (!pool || !disp || !disp->filename) return NULL;

  char *result = mem_alloc(pool, disp->filename_len + 1);
  if (!result) return NULL;

  memcpy(result, disp->filename, disp->filename_len);
  result[disp->filename_len] = '\0';
  return result;
}

char *mime_disposition_get_name(mem_pool_t *pool,
                                const mime_content_disposition_t *disp) {
  if (!pool || !disp || !disp->name) return NULL;

  char *result = mem_alloc(pool, disp->name_len + 1);
  if (!result) return NULL;

  memcpy(result, disp->name, disp->name_len);
  result[disp->name_len] = '\0';
  return result;
}
