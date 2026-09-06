#include "mime_parser.h"
#include "salts_simd_scan.h"
#include <string.h>
#include <ctype.h>
#include "salts_buffer.h"

/* ── Error messages ────────────────────────────────────────────────── */

const char *mime_errno_name(mime_errno_t err) {
  switch (err) {
    case MIME_OK: return "MIME_OK";
    case MIME_ERROR_INVALID_HEADER: return "MIME_ERROR_INVALID_HEADER";
    case MIME_ERROR_INVALID_BOUNDARY: return "MIME_ERROR_INVALID_BOUNDARY";
    case MIME_ERROR_MISSING_BOUNDARY: return "MIME_ERROR_MISSING_BOUNDARY";
    case MIME_ERROR_NESTED_TOO_DEEP: return "MIME_ERROR_NESTED_TOO_DEEP";
    case MIME_ERROR_MEMORY: return "MIME_ERROR_MEMORY";
    case MIME_ERROR_CALLBACK_FAILED: return "MIME_ERROR_CALLBACK_FAILED";
    default: return "UNKNOWN_ERROR";
  }
}

/* ── Helper functions ──────────────────────────────────────────────── */

const char *mime_extract_boundary(const char *content_type,
                                   size_t content_type_len,
                                   size_t *boundary_len) {
  if (!content_type || !boundary_len) return NULL;

  const char *content_type_end = content_type + content_type_len;
  const char *boundary_start = salts_scan_mem(content_type, content_type_len, "boundary=", 9);
  if (!boundary_start) return NULL;

  boundary_start += 9; // strlen("boundary=")
  if (boundary_start >= content_type_end) return NULL;

  // Skip quotes if present
  if (*boundary_start == '"') {
    boundary_start++;
    const char *end = salts_scan_char(boundary_start, content_type_end, '"');
    if (!end) return NULL;
    *boundary_len = end - boundary_start;
  } else {
    // Find end (semicolon or end of string)
    const char *end = salts_scan_to_any3(boundary_start, content_type_end, ';', '\r', '\n');
    *boundary_len = end - boundary_start;
  }

  return *boundary_len > 0 ? boundary_start : NULL;
}

int mime_is_multipart(const char *content_type, size_t len) {
  if (!content_type || len < 10) return 0;
  return strncmp(content_type, "multipart/", 10) == 0;
}

int mime_find_boundary(const char *data, size_t len,
                       const char *boundary, size_t boundary_len) {
  if (!data || !boundary || boundary_len == 0 || len < boundary_len) {
    return -1;
  }

  const char *hit = salts_scan_mem(data, len, boundary, boundary_len);
  return hit ? (int)(hit - data) : -1;
}

/* ── Parser implementation ─────────────────────────────────────────── */

void mime_parser_init(mime_parser_t *parser,
                      const mime_settings_t *settings,
                      mem_pool_t *pool) {
  memset(parser, 0, sizeof(*parser));
  if (settings) {
    parser->settings = *settings;
  }
  parser->pool = pool;
  parser->state = MIME_STATE_HEADERS;
}

void mime_parser_reset(mime_parser_t *parser) {
  mime_state_t old_state = parser->state;
  mime_settings_t settings = parser->settings;
  mem_pool_t *pool = parser->pool;
  void *data = parser->data;

  memset(parser, 0, sizeof(*parser));
  parser->settings = settings;
  parser->pool = pool;
  parser->data = data;
  parser->state = MIME_STATE_HEADERS;
}

/* ── Parsing helpers ───────────────────────────────────────────────── */

static int parse_header_line(mime_parser_t *parser, const char *line, size_t len) {
  // Find colon separator
  const char *colon = salts_scan_char(line, line + len, ':');
  if (!colon) {
    parser->error = MIME_ERROR_INVALID_HEADER;
    return -1;
  }

  size_t field_len = colon - line;
  const char *value = colon + 1;
  size_t value_len = len - field_len - 1;

  // Skip leading whitespace in value
  const char *value_end = value + value_len;
  value = salts_scan_skip_sp_tab(value, value_end);
  value_len = (size_t)(value_end - value);

  // Trim trailing whitespace
  while (value_len > 0 && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t')) {
    value_len--;
  }

  // Check for Content-Type to extract boundary string
  if (field_len == 12) {
    int is_content_type = 1;
    const char *expected = "Content-Type";
    for (size_t i = 0; i < 12; i++) {
      if (tolower((unsigned char)line[i]) != tolower((unsigned char)expected[i])) {
        is_content_type = 0;
        break;
      }
    }
    
    if (is_content_type && mime_is_multipart(value, value_len)) {
      if (parser->nesting_level >= MIME_MAX_NESTING) {
        parser->error = MIME_ERROR_NESTED_TOO_DEEP;
        return -1;
      }
      
      size_t b_len = 0;
      const char *b = mime_extract_boundary(value, value_len, &b_len);
      if (b && b_len > 0 && b_len + 2 <= MIME_MAX_BOUNDARY_LEN) {
        parser->boundary[0] = '-';
        parser->boundary[1] = '-';
        memcpy(&parser->boundary[2], b, b_len);
        parser->boundary_len = b_len + 2;
        parser->boundary[b_len + 2] = '\0';
        parser->nesting_level++;
      }
    }
  }

  // Call callbacks
  if (parser->settings.on_header_field) {
    if (parser->settings.on_header_field(parser, line, field_len) != 0) {
      parser->error = MIME_ERROR_CALLBACK_FAILED;
      return -1;
    }
  }

  if (parser->settings.on_header_value) {
    if (parser->settings.on_header_value(parser, value, value_len) != 0) {
      parser->error = MIME_ERROR_CALLBACK_FAILED;
      return -1;
    }
  }

  return 0;
}

static int parse_headers(mime_parser_t *parser, const char **data_ptr, size_t *len_ptr) {
  const char *line_start = *data_ptr;
  size_t len = *len_ptr;

  while (len > 0) {
    // Find line end
    const char *line_end = salts_scan_char(line_start, line_start + len, '\n');
    if (!line_end) {
      // Need more data
      *data_ptr = line_start;
      *len_ptr = len;
      return 0;
    }

    size_t line_len = line_end - line_start;
    size_t consumed = line_len + 1;

    // Handle \r\n
    if (line_len > 0 && line_start[line_len - 1] == '\r') {
      line_len--;
    }

    // Empty line = end of headers
    if (line_len == 0) {
      parser->state = MIME_STATE_HEADERS_DONE;
      *data_ptr = line_end + 1;
      *len_ptr = len - consumed;

      if (parser->settings.on_headers_complete) {
        if (parser->settings.on_headers_complete(parser) != 0) {
          parser->error = MIME_ERROR_CALLBACK_FAILED;
          return -1;
        }
      }
      return 1;
    }

    // Parse header line
    if (parse_header_line(parser, line_start, line_len) != 0) {
      return -1;
    }

    // Move to next line
    line_start = line_end + 1;
    len -= consumed;
  }

  *data_ptr = line_start;
  *len_ptr = len;
  return 0;
}

static int parse_body(mime_parser_t *parser, const char **data_ptr, size_t *len_ptr) {
  const char *data = *data_ptr;
  size_t len = *len_ptr;

  if (len == 0) return 0;

  // Check if multipart
  if (parser->boundary_len > 0) {
    // Find boundary
    int boundary_pos = mime_find_boundary(data, len, parser->boundary, parser->boundary_len);

    if (boundary_pos >= 0) {
      // Send body before boundary
      if (boundary_pos > 0 && parser->settings.on_body) {
        size_t body_len = boundary_pos;
        if (body_len >= 2 && data[body_len - 2] == '\r' && data[body_len - 1] == '\n') {
          body_len -= 2;
        } else if (body_len >= 1 && data[body_len - 1] == '\n') {
          body_len -= 1;
        }
        if (body_len > 0) {
          if (parser->settings.on_body(parser, data, body_len) != 0) {
            parser->error = MIME_ERROR_CALLBACK_FAILED;
            return -1;
          }
        }
      }

      // Move past boundary
      *data_ptr = data + boundary_pos + parser->boundary_len;
      *len_ptr = len - boundary_pos - parser->boundary_len;

      parser->state = MIME_STATE_BOUNDARY;
      return 1;
    }
  }

  // No boundary found, send all data as body
  if (parser->settings.on_body) {
    if (parser->settings.on_body(parser, data, len) != 0) {
      parser->error = MIME_ERROR_CALLBACK_FAILED;
      return -1;
    }
  }

  *data_ptr = data + len;
  *len_ptr = 0;
  parser->state = MIME_STATE_COMPLETE;

  if (parser->settings.on_message_complete) {
    if (parser->settings.on_message_complete(parser) != 0) {
      parser->error = MIME_ERROR_CALLBACK_FAILED;
      return -1;
    }
  }

  return 1;
}

mime_errno_t mime_parse(mime_parser_t *parser, const char *data, size_t len) {
  if (!parser || !data) return MIME_ERROR_MEMORY;

  const char *current = data;
  size_t remaining = len;

  while (remaining > 0 && parser->state != MIME_STATE_COMPLETE) {
    switch (parser->state) {
      case MIME_STATE_HEADERS: {
        int ret = parse_headers(parser, &current, &remaining);
        if (ret < 0) {
          return parser->error;
        }
        if (ret == 0) {
          // Need more data
          remaining = 0;
        }
        break;
      }

      case MIME_STATE_HEADERS_DONE:
        parser->state = MIME_STATE_BODY;
        break;

      case MIME_STATE_BODY:
        if (parse_body(parser, &current, &remaining) < 0) {
          return parser->error;
        }
        break;

      case MIME_STATE_BOUNDARY:
        // Consume \r\n if present after body
        if (remaining >= 2 && current[0] == '\r' && current[1] == '\n') {
          current += 2;
          remaining -= 2;
        } else if (remaining >= 1 && current[0] == '\n') {
          current += 1;
          remaining -= 1;
        }

        // Check for end boundary (--boundary--)
        if (remaining >= 2 && current[0] == '-' && current[1] == '-') {
          current += 2;
          remaining -= 2;
          // End of multipart
          if (parser->current_field && parser->settings.on_part_complete) {
            if (parser->settings.on_part_complete(parser) != 0) return MIME_ERROR_CALLBACK_FAILED;
          }
          parser->state = MIME_STATE_COMPLETE;
          if (parser->settings.on_message_complete) {
            if (parser->settings.on_message_complete(parser) != 0) {
              return MIME_ERROR_CALLBACK_FAILED;
            }
          }
        } else {
          // New part begins
          if (parser->current_field && parser->settings.on_part_complete) {
            if (parser->settings.on_part_complete(parser) != 0) return MIME_ERROR_CALLBACK_FAILED;
          }
          if (parser->settings.on_part_begin) {
            if (parser->settings.on_part_begin(parser) != 0) {
              return MIME_ERROR_CALLBACK_FAILED;
            }
          }
          parser->current_field = (const char*)1; // Mark that we are inside a part
          parser->state = MIME_STATE_HEADERS;
        }
        break;

      case MIME_STATE_COMPLETE:
        return MIME_OK;

      default:
        return MIME_ERROR_INVALID_HEADER;
    }
  }

  return parser->error;
}
