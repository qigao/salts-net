#include "websocket_handshake_parser.h"
#include "turbo_str.h"
#include <llhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Internal context for llhttp callbacks
typedef struct {
  websocket_handshake_parser_t *parser;
  websocket_handshake_token_value_t *current_value;
  tstr_t current_header_field;
  tstr_t current_header_value;
  int last_header_cb;
  int headers_complete;
} llhttp_context_t;

enum {
  LLHTTP_HDR_NONE = 0,
  LLHTTP_HDR_FIELD = 1,
  LLHTTP_HDR_VALUE = 2
};

static char *trim_inplace(char *s) {
  if (!s) return NULL;
  char *end;
  while (isspace((unsigned char)*s)) s++;
  if (*s == 0) return s;
  end = s + strlen(s) - 1;
  while (end > s && isspace((unsigned char)*end)) end--;
  end[1] = '\0';
  return s;
}

static void store_header_pair(llhttp_context_t *ctx) {
  websocket_handshake_parser_t *parser = ctx->parser;
  if (!ctx->current_header_field || !ctx->current_header_value)
    return;

  char *trimmed_value = trim_inplace(ctx->current_header_value);
  const char *field = ctx->current_header_field;

  if (tstr_casecmp(field, "Sec-WebSocket-Key") == 0) {
    tstr_free(parser->ws_key);
    parser->ws_key = tstr_dup(trimmed_value);
  } else if (tstr_casecmp(field, "Sec-WebSocket-Accept") == 0) {
    tstr_free(parser->ws_accept);
    parser->ws_accept = tstr_dup(trimmed_value);
  } else if (tstr_casecmp(field, "Sec-WebSocket-Version") == 0) {
    tstr_free(parser->ws_version);
    parser->ws_version = tstr_dup(trimmed_value);
  } else if (tstr_casecmp(field, "Sec-WebSocket-Protocol") == 0) {
    tstr_free(parser->ws_protocol);
    parser->ws_protocol = tstr_dup(trimmed_value);
  } else if (tstr_casecmp(field, "Upgrade") == 0) {
    tstr_free(parser->upgrade);
    parser->upgrade = tstr_dup(trimmed_value);
  } else if (tstr_casecmp(field, "Connection") == 0) {
    tstr_free(parser->connection);
    parser->connection = tstr_dup(trimmed_value);
  }
}

// llhttp callbacks
static int on_url(llhttp_t *http_parser, const char *at, size_t length) {
  llhttp_context_t *ctx = (llhttp_context_t *)http_parser->data;
  websocket_handshake_parser_t *parser = ctx->parser;
  parser->path_start = at;
  parser->path_len = length;
  return 0;
}

static int on_status(llhttp_t *http_parser, const char *at, size_t length) {
  llhttp_context_t *ctx = (llhttp_context_t *)http_parser->data;
  websocket_handshake_parser_t *parser = ctx->parser;
  parser->status_code = (int)http_parser->status_code;
  parser->status_text_start = at;
  parser->status_text_len = length;
  return 0;
}

static int on_header_field(llhttp_t *http_parser, const char *at, size_t length) {
  llhttp_context_t *ctx = (llhttp_context_t *)http_parser->data;
  if (ctx->last_header_cb == LLHTTP_HDR_VALUE) {
    store_header_pair(ctx);
    tstr_free(ctx->current_header_value);
    ctx->current_header_value = NULL;
  }
  if (ctx->last_header_cb != LLHTTP_HDR_FIELD) {
    tstr_free(ctx->current_header_field);
    ctx->current_header_field = NULL;
  }
  ctx->current_header_field =
      tstr_cat_len(ctx->current_header_field, at, length);
  if (!ctx->current_header_field) return -1;
  ctx->last_header_cb = LLHTTP_HDR_FIELD;
  return 0;
}

static int on_header_value(llhttp_t *http_parser, const char *at, size_t length) {
  llhttp_context_t *ctx = (llhttp_context_t *)http_parser->data;
  if (ctx->last_header_cb != LLHTTP_HDR_VALUE) {
    tstr_free(ctx->current_header_value);
    ctx->current_header_value = NULL;
  }
  ctx->current_header_value =
      tstr_cat_len(ctx->current_header_value, at, length);
  if (!ctx->current_header_value) return -1;
  ctx->last_header_cb = LLHTTP_HDR_VALUE;
  return 0;
}

static int on_headers_complete(llhttp_t *http_parser) {
  llhttp_context_t *ctx = (llhttp_context_t *)http_parser->data;
  if (ctx->last_header_cb == LLHTTP_HDR_VALUE) {
    store_header_pair(ctx);
  }
  ctx->headers_complete = 1;
  // Capture status code for responses even if on_status wasn't enough
  ctx->parser->status_code = (int)http_parser->status_code;
  return 0;
}

void websocket_handshake_parser_init(websocket_handshake_parser_t *parser,
                                   websocket_handshake_mode_t mode,
                                   const char *data, size_t len) {
  memset(parser, 0, sizeof(*parser));
  parser->data = data;
  parser->limit = data + len;
  parser->cursor = data;
  parser->start = data;
  parser->mode = mode;
  parser->state = WEBSOCKET_HANDSHAKE_STATE_REQUEST_LINE;

  parser->http_parser = calloc(1, sizeof(llhttp_t));
  if (!parser->http_parser) return;

  parser->http_context = calloc(1, sizeof(llhttp_context_t));
  if (!parser->http_context) {
    free(parser->http_parser);
    parser->http_parser = NULL;
    return;
  }

  llhttp_context_t *ctx = (llhttp_context_t *)parser->http_context;
  ctx->parser = parser;

  llhttp_settings_t *settings = calloc(1, sizeof(llhttp_settings_t));
  parser->http_settings = settings;
  llhttp_settings_init(settings);
  settings->on_url = on_url;
  settings->on_status = on_status;
  settings->on_header_field = on_header_field;
  settings->on_header_value = on_header_value;
  settings->on_headers_complete = on_headers_complete;

  llhttp_init((llhttp_t *)parser->http_parser, 
             (mode == WEBSOCKET_HANDSHAKE_MODE_REQUEST) ? HTTP_REQUEST : HTTP_RESPONSE, 
             settings);

  ((llhttp_t *)parser->http_parser)->data = ctx;
}

int websocket_handshake_parser_scan(websocket_handshake_parser_t *parser,
                                   websocket_handshake_token_value_t *value) {
  if (!parser->http_parser || !parser->http_context) return WEBSOCKET_HANDSHAKE_TOKEN_ERROR;
  if (parser->state == WEBSOCKET_HANDSHAKE_STATE_COMPLETE) return WEBSOCKET_HANDSHAKE_TOKEN_END;
  if (parser->state == WEBSOCKET_HANDSHAKE_STATE_ERROR) return WEBSOCKET_HANDSHAKE_TOKEN_ERROR;

  llhttp_t *http_parser = (llhttp_t *)parser->http_parser;
  llhttp_context_t *ctx = (llhttp_context_t *)parser->http_context;
  ctx->current_value = value;

  size_t remaining = parser->limit - parser->cursor;
  enum llhttp_errno err = llhttp_execute(http_parser, parser->cursor, remaining);
  
  if (err == HPE_OK || err == HPE_PAUSED_UPGRADE) {
    parser->cursor += remaining;
  }
  
  const char *err_pos = llhttp_get_error_pos(http_parser);
  if (err_pos) parser->cursor = err_pos;

  if (err == HPE_OK || err == HPE_PAUSED_UPGRADE) {
    parser->state = WEBSOCKET_HANDSHAKE_STATE_COMPLETE;
    parser->version_start = "HTTP/1.1";
    parser->version_len = 8;

    if (parser->mode == WEBSOCKET_HANDSHAKE_MODE_REQUEST) {
      const char *method_name = llhttp_method_name(http_parser->method);
      parser->method_start = method_name;
      parser->method_len = strlen(method_name);
      value->request_line.method = parser->method_start;
      value->request_line.method_len = parser->method_len;
      value->request_line.path = parser->path_start;
      value->request_line.path_len = parser->path_len;
      value->request_line.version = parser->version_start;
      value->request_line.version_len = parser->version_len;
      return WEBSOCKET_HANDSHAKE_TOKEN_REQUEST_LINE;
    } else {
      value->response_line.status_code = parser->status_code;
      value->response_line.status_text = parser->status_text_start;
      value->response_line.status_text_len = parser->status_text_len;
      value->response_line.version = parser->version_start;
      value->response_line.version_len = parser->version_len;
      return WEBSOCKET_HANDSHAKE_TOKEN_RESPONSE_LINE;
    }
  } else {
    parser->state = WEBSOCKET_HANDSHAKE_STATE_ERROR;
    return WEBSOCKET_HANDSHAKE_TOKEN_ERROR;
  }
}

void websocket_handshake_parser_reset(websocket_handshake_parser_t *parser,
                                    const char *data, size_t len) {
  websocket_handshake_mode_t mode = parser->mode;
  websocket_handshake_parser_destroy(parser);
  websocket_handshake_parser_init(parser, mode, data, len);
}

void websocket_handshake_parser_destroy(websocket_handshake_parser_t *parser) {
  if (!parser) return;
  if (parser->http_context) {
    llhttp_context_t *ctx = (llhttp_context_t *)parser->http_context;
    tstr_free(ctx->current_header_field);
    tstr_free(ctx->current_header_value);
    free(ctx);
  }
  free(parser->http_settings);
  free(parser->http_parser);
  tstr_free(parser->ws_key);
  tstr_free(parser->ws_version);
  tstr_free(parser->ws_protocol);
  tstr_free(parser->ws_accept);
  tstr_free(parser->upgrade);
  tstr_free(parser->connection);
  memset(parser, 0, sizeof(*parser));
}

int websocket_handshake_parser_is_complete(const websocket_handshake_parser_t *parser) {
  return parser->state == WEBSOCKET_HANDSHAKE_STATE_COMPLETE;
}

size_t websocket_handshake_parser_bytes_consumed(const websocket_handshake_parser_t *parser) {
  if (!parser || !parser->data || !parser->cursor) return 0;
  return (size_t)(parser->cursor - parser->data);
}

int websocket_handshake_validate(const websocket_handshake_parser_t *parser) {
  if (!websocket_handshake_parser_is_complete(parser)) return -1;
  if (parser->mode == WEBSOCKET_HANDSHAKE_MODE_REQUEST) {
    if (parser->method_len != 3 || memcmp(parser->method_start, "GET", 3) != 0) return -2;
  } else {
    if (parser->status_code != 101) return -2;
  }
  return 0;
}

int websocket_handshake_get_key(const websocket_handshake_parser_t *parser,
                                char *key_buffer, size_t buffer_size) {
  const char *target = (parser->mode == WEBSOCKET_HANDSHAKE_MODE_REQUEST) ? parser->ws_key : parser->ws_accept;
  if (!target || !key_buffer || buffer_size == 0) return -1;
  size_t len = tstr_len(target);
  if (len >= buffer_size) return -1;
  memcpy(key_buffer, target, len);
  key_buffer[len] = '\0';
  return 0;
}

int websocket_handshake_is_websocket_request(const websocket_handshake_parser_t *parser) {
  if (!parser || !websocket_handshake_parser_is_complete(parser)) return 0;
  if (!parser->upgrade || !parser->connection) return 0;
  
  if (tstr_casecmp(parser->upgrade, "websocket") != 0) return 0;
  
  // Check Connection header for upgrade
  int has_upgrade = 0;
  tstr_t conn_lower = tstr_dup(parser->connection);
  if (conn_lower) {
    tstr_lower(conn_lower);
    if (tstr_contains(conn_lower, "upgrade")) {
      has_upgrade = 1;
    }
    tstr_free(conn_lower);
  }
  
  if (!has_upgrade) return 0;

  if (parser->mode == WEBSOCKET_HANDSHAKE_MODE_REQUEST) {
    if (!parser->ws_key || !parser->ws_version || strcmp(parser->ws_version, "13") != 0) return 0;
  } else {
    if (!parser->ws_accept) return 0;
  }
  return 1;
}
