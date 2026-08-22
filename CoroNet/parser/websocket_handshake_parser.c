#include "websocket_handshake_parser.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int ascii_equal_case(const char *left, size_t left_len,
                            const char *right) {
  size_t i;
  size_t right_len;

  if (!left || !right) return 0;
  right_len = strlen(right);
  if (left_len != right_len) return 0;
  for (i = 0; i < left_len; ++i) {
    if (tolower((unsigned char)left[i]) !=
        tolower((unsigned char)right[i])) {
      return 0;
    }
  }
  return 1;
}

static int is_header_name_char(unsigned char ch) {
  if (isalnum(ch)) return 1;
  switch (ch) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return 1;
    default:
      return 0;
  }
}

static const char *find_crlf(const char *cursor, const char *limit) {
  while (cursor && cursor + 1 < limit) {
    if (cursor[0] == '\r' && cursor[1] == '\n') return cursor;
    ++cursor;
  }
  return NULL;
}

static char *duplicate_span(const char *start, size_t len) {
  char *copy = (char *)malloc(len + 1U);
  if (!copy) return NULL;
  if (len > 0) memcpy(copy, start, len);
  copy[len] = '\0';
  return copy;
}

static int set_unique_header(char **slot, const char *value, size_t value_len) {
  if (*slot) return -1;
  *slot = duplicate_span(value, value_len);
  return *slot ? 0 : -1;
}

static int append_connection_header(char **slot, const char *value,
                                    size_t value_len) {
  char *combined;
  size_t old_len;

  if (!*slot) return set_unique_header(slot, value, value_len);
  old_len = strlen(*slot);
  if (old_len > SIZE_MAX - value_len - 2U) return -1;
  combined = (char *)malloc(old_len + value_len + 2U);
  if (!combined) return -1;
  memcpy(combined, *slot, old_len);
  combined[old_len] = ',';
  memcpy(combined + old_len + 1U, value, value_len);
  combined[old_len + value_len + 1U] = '\0';
  free(*slot);
  *slot = combined;
  return 0;
}

static int store_header(websocket_handshake_parser_t *parser,
                        const char *name, size_t name_len, const char *value,
                        size_t value_len) {
  if (ascii_equal_case(name, name_len, "Sec-WebSocket-Key")) {
    return set_unique_header(&parser->ws_key, value, value_len);
  }
  if (ascii_equal_case(name, name_len, "Sec-WebSocket-Accept")) {
    return set_unique_header(&parser->ws_accept, value, value_len);
  }
  if (ascii_equal_case(name, name_len, "Sec-WebSocket-Version")) {
    return set_unique_header(&parser->ws_version, value, value_len);
  }
  if (ascii_equal_case(name, name_len, "Sec-WebSocket-Protocol")) {
    return set_unique_header(&parser->ws_protocol, value, value_len);
  }
  if (ascii_equal_case(name, name_len, "Upgrade")) {
    return set_unique_header(&parser->upgrade, value, value_len);
  }
  if (ascii_equal_case(name, name_len, "Connection")) {
    return append_connection_header(&parser->connection, value, value_len);
  }
  return 0;
}

static int parse_request_line(websocket_handshake_parser_t *parser,
                              const char *line, const char *line_end) {
  const char *method_end;
  const char *path_end;

  method_end = (const char *)memchr(line, ' ', (size_t)(line_end - line));
  if (!method_end || method_end == line) return -1;
  path_end = (const char *)memchr(method_end + 1, ' ',
                                 (size_t)(line_end - method_end - 1));
  if (!path_end || path_end == method_end + 1) return -1;
  if ((size_t)(line_end - path_end - 1) != sizeof("HTTP/1.1") - 1U ||
      memcmp(path_end + 1, "HTTP/1.1", sizeof("HTTP/1.1") - 1U) != 0) {
    return -1;
  }

  parser->method_start = line;
  parser->method_len = (size_t)(method_end - line);
  parser->path_start = method_end + 1;
  parser->path_len = (size_t)(path_end - method_end - 1);
  parser->version_start = path_end + 1;
  parser->version_len = sizeof("HTTP/1.1") - 1U;
  return 0;
}

static int parse_response_line(websocket_handshake_parser_t *parser,
                               const char *line, const char *line_end) {
  const char *status;

  if ((size_t)(line_end - line) < sizeof("HTTP/1.1 101") - 1U ||
      memcmp(line, "HTTP/1.1 ", sizeof("HTTP/1.1 ") - 1U) != 0) {
    return -1;
  }
  status = line + sizeof("HTTP/1.1 ") - 1U;
  if (status + 3 > line_end || !isdigit((unsigned char)status[0]) ||
      !isdigit((unsigned char)status[1]) ||
      !isdigit((unsigned char)status[2]) ||
      (status + 3 < line_end && status[3] != ' ')) {
    return -1;
  }

  parser->version_start = line;
  parser->version_len = sizeof("HTTP/1.1") - 1U;
  parser->status_code = (status[0] - '0') * 100 + (status[1] - '0') * 10 +
                        (status[2] - '0');
  if (status + 3 < line_end) {
    parser->status_text_start = status + 4;
    parser->status_text_len = (size_t)(line_end - status - 4);
  }
  return 0;
}

static int parse_header_line(websocket_handshake_parser_t *parser,
                             const char *line, const char *line_end) {
  const char *colon;
  const char *value;
  const char *value_end;
  const char *cursor;

  if (line == line_end || *line == ' ' || *line == '\t') return -1;
  colon = (const char *)memchr(line, ':', (size_t)(line_end - line));
  if (!colon || colon == line) return -1;
  for (cursor = line; cursor < colon; ++cursor) {
    if (!is_header_name_char((unsigned char)*cursor)) return -1;
  }

  value = colon + 1;
  while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
  value_end = line_end;
  while (value_end > value &&
         (value_end[-1] == ' ' || value_end[-1] == '\t')) {
    --value_end;
  }
  for (cursor = value; cursor < value_end; ++cursor) {
    unsigned char ch = (unsigned char)*cursor;
    if (ch == 0 || ch == '\r' || ch == '\n' ||
        (ch < 0x20U && ch != '\t') || ch == 0x7fU) {
      return -1;
    }
  }

  parser->header_name_start = line;
  parser->header_name_len = (size_t)(colon - line);
  parser->header_value_start = value;
  parser->header_value_len = (size_t)(value_end - value);
  return store_header(parser, line, (size_t)(colon - line), value,
                      (size_t)(value_end - value));
}

static int parse_handshake(websocket_handshake_parser_t *parser) {
  const char *cursor;
  const char *line_end;

  if (!parser || !parser->data || !parser->limit ||
      parser->limit < parser->data) {
    return -1;
  }
  cursor = parser->data;
  line_end = find_crlf(cursor, parser->limit);
  if (!line_end) return -1;
  if (parser->mode == WEBSOCKET_HANDSHAKE_MODE_REQUEST) {
    if (parse_request_line(parser, cursor, line_end) != 0) return -1;
  } else if (parse_response_line(parser, cursor, line_end) != 0) {
    return -1;
  }
  cursor = line_end + 2;

  while (cursor < parser->limit) {
    line_end = find_crlf(cursor, parser->limit);
    if (!line_end) return -1;
    if (line_end == cursor) {
      parser->cursor = line_end + 2;
      return 0;
    }
    if (parse_header_line(parser, cursor, line_end) != 0) return -1;
    cursor = line_end + 2;
  }
  return -1;
}

static int header_has_token(const char *value, const char *token) {
  const char *cursor;

  if (!value || !token || !token[0]) return 0;
  cursor = value;
  while (*cursor) {
    const char *start;
    const char *end;

    while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') ++cursor;
    start = cursor;
    while (*cursor && *cursor != ',') ++cursor;
    end = cursor;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;
    if (ascii_equal_case(start, (size_t)(end - start), token)) return 1;
    if (*cursor == ',') ++cursor;
  }
  return 0;
}

void websocket_handshake_parser_init(websocket_handshake_parser_t *parser,
                                     websocket_handshake_mode_t mode,
                                     const char *data, size_t len) {
  if (!parser) return;
  memset(parser, 0, sizeof(*parser));
  parser->data = data;
  parser->limit = data ? data + len : NULL;
  parser->cursor = data;
  parser->start = data;
  parser->mode = mode;
  parser->state = data ? WEBSOCKET_HANDSHAKE_STATE_REQUEST_LINE
                       : WEBSOCKET_HANDSHAKE_STATE_ERROR;
}

int websocket_handshake_parser_scan(websocket_handshake_parser_t *parser,
                                    websocket_handshake_token_value_t *value) {
  if (!parser || !value) return WEBSOCKET_HANDSHAKE_TOKEN_ERROR;
  if (parser->state == WEBSOCKET_HANDSHAKE_STATE_COMPLETE) {
    return WEBSOCKET_HANDSHAKE_TOKEN_END;
  }
  if (parser->state == WEBSOCKET_HANDSHAKE_STATE_ERROR ||
      parse_handshake(parser) != 0) {
    parser->state = WEBSOCKET_HANDSHAKE_STATE_ERROR;
    return WEBSOCKET_HANDSHAKE_TOKEN_ERROR;
  }

  parser->state = WEBSOCKET_HANDSHAKE_STATE_COMPLETE;
  memset(value, 0, sizeof(*value));
  if (parser->mode == WEBSOCKET_HANDSHAKE_MODE_REQUEST) {
    value->request_line.method = parser->method_start;
    value->request_line.method_len = parser->method_len;
    value->request_line.path = parser->path_start;
    value->request_line.path_len = parser->path_len;
    value->request_line.version = parser->version_start;
    value->request_line.version_len = parser->version_len;
    return WEBSOCKET_HANDSHAKE_TOKEN_REQUEST_LINE;
  }

  value->response_line.status_code = parser->status_code;
  value->response_line.status_text = parser->status_text_start;
  value->response_line.status_text_len = parser->status_text_len;
  value->response_line.version = parser->version_start;
  value->response_line.version_len = parser->version_len;
  return WEBSOCKET_HANDSHAKE_TOKEN_RESPONSE_LINE;
}

void websocket_handshake_parser_reset(websocket_handshake_parser_t *parser,
                                      const char *data, size_t len) {
  websocket_handshake_mode_t mode;
  if (!parser) return;
  mode = parser->mode;
  websocket_handshake_parser_destroy(parser);
  websocket_handshake_parser_init(parser, mode, data, len);
}

void websocket_handshake_parser_destroy(websocket_handshake_parser_t *parser) {
  if (!parser) return;
  free(parser->ws_key);
  free(parser->ws_version);
  free(parser->ws_protocol);
  free(parser->ws_accept);
  free(parser->upgrade);
  free(parser->connection);
  memset(parser, 0, sizeof(*parser));
}

int websocket_handshake_parser_is_complete(
    const websocket_handshake_parser_t *parser) {
  return parser && parser->state == WEBSOCKET_HANDSHAKE_STATE_COMPLETE;
}

size_t websocket_handshake_parser_bytes_consumed(
    const websocket_handshake_parser_t *parser) {
  if (!parser || !parser->data || !parser->cursor) return 0;
  return (size_t)(parser->cursor - parser->data);
}

int websocket_handshake_validate(
    const websocket_handshake_parser_t *parser) {
  if (!websocket_handshake_parser_is_complete(parser)) return -1;
  if (parser->mode == WEBSOCKET_HANDSHAKE_MODE_REQUEST) {
    if (parser->method_len != sizeof("GET") - 1U ||
        memcmp(parser->method_start, "GET", sizeof("GET") - 1U) != 0) {
      return -2;
    }
  } else if (parser->status_code != 101) {
    return -2;
  }
  return 0;
}

int websocket_handshake_get_key(
    const websocket_handshake_parser_t *parser, char *key_buffer,
    size_t buffer_size) {
  const char *target;
  size_t len;

  if (!parser || !key_buffer || buffer_size == 0) return -1;
  target = parser->mode == WEBSOCKET_HANDSHAKE_MODE_REQUEST
               ? parser->ws_key
               : parser->ws_accept;
  if (!target) return -1;
  len = strlen(target);
  if (len >= buffer_size) return -1;
  memcpy(key_buffer, target, len + 1U);
  return 0;
}

int websocket_handshake_is_websocket_request(
    const websocket_handshake_parser_t *parser) {
  if (!websocket_handshake_parser_is_complete(parser) || !parser->upgrade ||
      !parser->connection ||
      !header_has_token(parser->upgrade, "websocket") ||
      !header_has_token(parser->connection, "upgrade")) {
    return 0;
  }

  if (parser->mode == WEBSOCKET_HANDSHAKE_MODE_REQUEST) {
    return parser->ws_key && parser->ws_key[0] && parser->ws_version &&
           strcmp(parser->ws_version, "13") == 0;
  }
  return parser->ws_accept && parser->ws_accept[0];
}
