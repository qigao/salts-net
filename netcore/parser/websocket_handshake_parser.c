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
  int request_line_complete;
  int headers_complete;
} llhttp_context_t;

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

  free(ctx->current_header_field);
  ctx->current_header_field = malloc(length + 1);
  if (!ctx->current_header_field) return -1;

  memcpy(ctx->current_header_field, at, length);
  ctx->current_header_field[length] = '\0';
  return 0;
}

static int on_header_value(llhttp_t *http_parser, const char *at, size_t length) {
  llhttp_context_t *ctx = (llhttp_context_t *)http_parser->data;
  websocket_handshake_parser_t *parser = ctx->parser;

  free(ctx->current_header_value);
  ctx->current_header_value = malloc(length + 1);
  if (!ctx->current_header_value) return -1;

  memcpy(ctx->current_header_value, at, length);
  ctx->current_header_value[length] = '\0';

  // Store WebSocket-specific headers
  if (ctx->current_header_field) {
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

  return 0;
}

static int on_headers_complete(llhttp_t *http_parser) {
  llhttp_context_t *ctx = (llhttp_context_t *)http_parser->data;
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
  llhttp_settings_t *settings = (llhttp_settings_t *)parser->http_settings;
  llhttp_context_t *ctx = (llhttp_context_t *)parser->http_context;

  ctx->current_value = value;

  // Parse available data
  size_t remaining = parser->limit - parser->cursor;
  enum llhttp_errno err = llhttp_execute(http_parser, parser->cursor, remaining);

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
  if (!parser->http_parser) return 0;

  llhttp_t *http_parser = (llhttp_t *)parser->http_parser;
  return (size_t)((const char *)llhttp_get_error_pos(http_parser) - parser->data);
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
    *p = tolower((unsigned char)*p);
  }

  int has_upgrade = strstr(conn_lower, "upgrade") != NULL;
  free(conn_lower);

  if (!has_upgrade) return 0;

  // Check WebSocket version (13 is the standard)
  if (strcmp(parser->ws_version, "13") != 0) return 0;

  return 1; // Valid WebSocket request
}
