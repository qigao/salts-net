/**
 * @file websocket_handshake_parser.c
 * @brief WebSocket Handshake Parser using llhttp
 */

#include "websocket_handshake_parser.h"
#include <llhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#define strdup _strdup
#endif

// Internal context for llhttp callbacks
typedef struct {
  websocket_handshake_parser_t *parser;
  websocket_handshake_token_value_t *current_value;
  char *current_header_field;
  char *current_header_value;
  size_t current_header_field_len;
  size_t current_header_value_len;
  int last_header_cb;
  int request_line_complete;
  int headers_complete;
} llhttp_context_t;

enum {
  LLHTTP_HDR_NONE = 0,
  LLHTTP_HDR_FIELD = 1,
  LLHTTP_HDR_VALUE = 2
};

static int append_header_part(char **buffer, size_t *len, const char *at, size_t length) {
  char *new_buf = (char *)realloc(*buffer, *len + length + 1);
  if (!new_buf)
    return -1;
  memcpy(new_buf + *len, at, length);
  *len += length;
  new_buf[*len] = '\0';
  *buffer = new_buf;
  return 0;
}

static void store_header_pair(llhttp_context_t *ctx) {
  websocket_handshake_parser_t *parser = ctx->parser;
  if (!ctx->current_header_field || !ctx->current_header_value)
    return;

  const char *field = ctx->current_header_field;

  if (strcasecmp(field, "Sec-WebSocket-Key") == 0) {
    free(parser->ws_key);
    parser->ws_key = strdup(ctx->current_header_value);
  } else if (strcasecmp(field, "Sec-WebSocket-Version") == 0) {
    free(parser->ws_version);
    parser->ws_version = strdup(ctx->current_header_value);
  } else if (strcasecmp(field, "Sec-WebSocket-Protocol") == 0) {
    free(parser->ws_protocol);
    parser->ws_protocol = strdup(ctx->current_header_value);
  } else if (strcasecmp(field, "Upgrade") == 0) {
    free(parser->upgrade);
    parser->upgrade = strdup(ctx->current_header_value);
  } else if (strcasecmp(field, "Connection") == 0) {
    free(parser->connection);
    parser->connection = strdup(ctx->current_header_value);
  }
}

// llhttp callbacks for HTTP request parsing
static int on_url(llhttp_t *http_parser, const char *at, size_t length) {
  llhttp_context_t *ctx = (llhttp_context_t *)http_parser->data;
  websocket_handshake_parser_t *parser = ctx->parser;

  parser->path_start = at;
  parser->path_len = length;
  return 0;
}

static int on_header_field(llhttp_t *http_parser, const char *at, size_t length) {
  llhttp_context_t *ctx = (llhttp_context_t *)http_parser->data;

  if (ctx->last_header_cb == LLHTTP_HDR_VALUE) {
    store_header_pair(ctx);
    free(ctx->current_header_value);
    ctx->current_header_value = NULL;
    ctx->current_header_value_len = 0;
  }

  if (ctx->last_header_cb != LLHTTP_HDR_FIELD) {
    free(ctx->current_header_field);
    ctx->current_header_field = NULL;
    ctx->current_header_field_len = 0;
  }

  if (append_header_part(&ctx->current_header_field, &ctx->current_header_field_len, at, length) != 0)
    return -1;

  ctx->last_header_cb = LLHTTP_HDR_FIELD;
  return 0;
}

static int on_header_value(llhttp_t *http_parser, const char *at, size_t length) {
  llhttp_context_t *ctx = (llhttp_context_t *)http_parser->data;
  if (ctx->last_header_cb != LLHTTP_HDR_VALUE) {
    free(ctx->current_header_value);
    ctx->current_header_value = NULL;
    ctx->current_header_value_len = 0;
  }

  if (append_header_part(&ctx->current_header_value, &ctx->current_header_value_len, at, length) != 0)
    return -1;

  ctx->last_header_cb = LLHTTP_HDR_VALUE;

  return 0;
}

static int on_headers_complete(llhttp_t *http_parser) {
  llhttp_context_t *ctx = (llhttp_context_t *)http_parser->data;
  if (ctx->last_header_cb == LLHTTP_HDR_VALUE) {
    store_header_pair(ctx);
  }
  ctx->headers_complete = 1;
  return 0;
}

void websocket_handshake_parser_init(websocket_handshake_parser_t *parser,
                                   const char *data, size_t len) {
  memset(parser, 0, sizeof(*parser));
  parser->data = data;
  parser->limit = data + len;
  parser->cursor = data;
  parser->start = data;
  parser->state = WEBSOCKET_HANDSHAKE_STATE_REQUEST_LINE;

  // Initialize llhttp parser for HTTP request
  parser->http_parser = calloc(1, sizeof(llhttp_t));
  if (!parser->http_parser) return;

  llhttp_init((llhttp_t *)parser->http_parser, HTTP_REQUEST, NULL);

  // Initialize context
  parser->http_context = calloc(1, sizeof(llhttp_context_t));
  if (!parser->http_context) {
    free(parser->http_parser);
    parser->http_parser = NULL;
    return;
  }

  llhttp_context_t *ctx = (llhttp_context_t *)parser->http_context;
  ctx->parser = parser;
  ((llhttp_t *)parser->http_parser)->data = ctx;

  // Set callbacks
  llhttp_settings_t *settings = calloc(1, sizeof(llhttp_settings_t));
  parser->http_settings = settings;

  llhttp_settings_init(settings);
  settings->on_url = on_url;
  settings->on_header_field = on_header_field;
  settings->on_header_value = on_header_value;
  settings->on_headers_complete = on_headers_complete;
}

int websocket_handshake_parser_scan(websocket_handshake_parser_t *parser,
                                  websocket_handshake_token_value_t *value) {
  if (!parser->http_parser || !parser->http_context) {
    return WEBSOCKET_HANDSHAKE_TOKEN_ERROR;
  }

  if (parser->state == WEBSOCKET_HANDSHAKE_STATE_COMPLETE)
    return WEBSOCKET_HANDSHAKE_TOKEN_END;
  if (parser->state == WEBSOCKET_HANDSHAKE_STATE_ERROR)
    return WEBSOCKET_HANDSHAKE_TOKEN_ERROR;

  llhttp_t *http_parser = (llhttp_t *)parser->http_parser;
  llhttp_context_t *ctx = (llhttp_context_t *)parser->http_context;
  (void)parser->http_settings;

  ctx->current_value = value;

  // Parse available data
  size_t remaining = parser->limit - parser->cursor;
  enum llhttp_errno err = llhttp_execute(http_parser, parser->cursor, remaining);
  if (err == HPE_OK || err == HPE_PAUSED_UPGRADE) {
    parser->cursor = parser->limit;
  } else if (err != HPE_OK) {
    const char *err_pos = llhttp_get_error_pos(http_parser);
    if (err_pos && err_pos >= parser->data && err_pos <= parser->limit) {
      parser->cursor = err_pos;
    }
  }

  if (err == HPE_OK) {
    // Parsing complete
    parser->state = WEBSOCKET_HANDSHAKE_STATE_COMPLETE;

    // Fill request line info
    const char *method_name = llhttp_method_name(http_parser->method);
    parser->method_start = method_name;
    parser->method_len = strlen(method_name);

    // HTTP version
    parser->version_start = "HTTP/1.1"; // llhttp provides this
    parser->version_len = 8;

    value->request_line.method = parser->method_start;
    value->request_line.method_len = parser->method_len;
    value->request_line.path = parser->path_start;
    value->request_line.path_len = parser->path_len;
    value->request_line.version = parser->version_start;
    value->request_line.version_len = parser->version_len;

    return WEBSOCKET_HANDSHAKE_TOKEN_REQUEST_LINE;
  } else if (err == HPE_PAUSED_UPGRADE) {
    // Upgrade request (WebSocket), headers complete
    parser->state = WEBSOCKET_HANDSHAKE_STATE_COMPLETE;
    return WEBSOCKET_HANDSHAKE_TOKEN_END;
  } else if (err != HPE_OK && llhttp_get_errno(http_parser) != HPE_OK) {
    // Parsing error
    parser->state = WEBSOCKET_HANDSHAKE_STATE_ERROR;
    return WEBSOCKET_HANDSHAKE_TOKEN_ERROR;
  }

  // Need more data
  return WEBSOCKET_HANDSHAKE_TOKEN_NEED_MORE;
}

void websocket_handshake_parser_reset(websocket_handshake_parser_t *parser,
                                    const char *data, size_t len) {
  // Clean up old state
  if (parser->http_context) {
    llhttp_context_t *ctx = (llhttp_context_t *)parser->http_context;
    free(ctx->current_header_field);
    free(ctx->current_header_value);
    free(ctx);
  }
  if (parser->http_settings) {
    free(parser->http_settings);
  }
  if (parser->http_parser) {
    free(parser->http_parser);
  }

  // Free stored headers
  free(parser->ws_key);
  free(parser->ws_version);
  free(parser->ws_protocol);
  free(parser->upgrade);
  free(parser->connection);

  // Reinitialize
  websocket_handshake_parser_init(parser, data, len);
}

void websocket_handshake_parser_destroy(websocket_handshake_parser_t *parser) {
  if (!parser) return;

  // Clean up llhttp state
  if (parser->http_context) {
    llhttp_context_t *ctx = (llhttp_context_t *)parser->http_context;
    free(ctx->current_header_field);
    free(ctx->current_header_value);
    free(ctx);
  }
  if (parser->http_settings) {
    free(parser->http_settings);
  }
  if (parser->http_parser) {
    free(parser->http_parser);
  }

  // Free stored headers
  free(parser->ws_key);
  free(parser->ws_version);
  free(parser->ws_protocol);
  free(parser->upgrade);
  free(parser->connection);

  // Clear structure
  memset(parser, 0, sizeof(*parser));
}

int websocket_handshake_parser_is_complete(const websocket_handshake_parser_t *parser) {
  return parser->state == WEBSOCKET_HANDSHAKE_STATE_COMPLETE;
}

size_t websocket_handshake_parser_bytes_consumed(const websocket_handshake_parser_t *parser) {
  if (!parser || !parser->data || !parser->cursor)
    return 0;
  if (parser->cursor < parser->data)
    return 0;
  return (size_t)(parser->cursor - parser->data);
}

int websocket_handshake_validate(const websocket_handshake_parser_t *parser) {
  if (!websocket_handshake_parser_is_complete(parser)) {
    return -1; // Not complete
  }

  // Validate method is GET
  if (parser->method_len != 3 || memcmp(parser->method_start, "GET", 3) != 0) {
    return -2; // Not a GET request
  }

  return 0; // Valid
}

int websocket_handshake_get_key(const websocket_handshake_parser_t *parser,
                              char *key_buffer, size_t buffer_size) {
  if (!parser || !key_buffer || buffer_size == 0) {
    return -1;
  }

  if (!parser->ws_key) {
    return -1; // No Sec-WebSocket-Key header found
  }

  size_t key_len = strlen(parser->ws_key);
  if (key_len >= buffer_size) {
    return -1; // Buffer too small
  }

  memcpy(key_buffer, parser->ws_key, key_len);
  key_buffer[key_len] = '\0';
  return 0;
}

int websocket_handshake_is_websocket_request(const websocket_handshake_parser_t *parser) {
  if (!parser) return 0;

  // Check required WebSocket headers
  if (!parser->ws_key) return 0;
  if (!parser->ws_version) return 0;
  if (!parser->upgrade) return 0;
  if (!parser->connection) return 0;

  // Validate header values
  if (strcasecmp(parser->upgrade, "websocket") != 0) return 0;

  // Connection should contain "Upgrade" (case-insensitive)
  // Simple check: look for "upgrade" substring
  char *conn_lower = strdup(parser->connection);
  if (!conn_lower) return 0;

  for (char *p = conn_lower; *p; p++) {
    *p = (char)tolower((unsigned char)*p);
  }

  int has_upgrade = strstr(conn_lower, "upgrade") != NULL;
  free(conn_lower);

  if (!has_upgrade) return 0;

  // Check WebSocket version (13 is the standard)
  if (strcmp(parser->ws_version, "13") != 0) return 0;

  return 1; // Valid WebSocket request
}
