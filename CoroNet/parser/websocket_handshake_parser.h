/**
 * @file websocket_handshake_parser.h
 * @brief WebSocket Handshake Parser API
 */

#ifndef WEBSOCKET_HANDSHAKE_PARSER_H
#define WEBSOCKET_HANDSHAKE_PARSER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WebSocket handshake parser states
 */
typedef enum {
  WEBSOCKET_HANDSHAKE_STATE_REQUEST_LINE,
  WEBSOCKET_HANDSHAKE_STATE_HEADERS,
  WEBSOCKET_HANDSHAKE_STATE_COMPLETE,
  WEBSOCKET_HANDSHAKE_STATE_ERROR
} websocket_handshake_state_t;

/**
 * @brief WebSocket handshake parser token types
 */
typedef enum {
  WEBSOCKET_HANDSHAKE_TOKEN_REQUEST_LINE,
  WEBSOCKET_HANDSHAKE_TOKEN_RESPONSE_LINE,
  WEBSOCKET_HANDSHAKE_TOKEN_HEADER,
  WEBSOCKET_HANDSHAKE_TOKEN_BODY,
  WEBSOCKET_HANDSHAKE_TOKEN_END,
  WEBSOCKET_HANDSHAKE_TOKEN_ERROR,
  WEBSOCKET_HANDSHAKE_TOKEN_NEED_MORE
} websocket_handshake_token_type_t;

/**
 * @brief WebSocket handshake parser modes
 */
typedef enum {
  WEBSOCKET_HANDSHAKE_MODE_REQUEST,
  WEBSOCKET_HANDSHAKE_MODE_RESPONSE
} websocket_handshake_mode_t;

/**
 * @brief WebSocket handshake token value union
 */
typedef union {
  struct {
    const char *method;
    size_t method_len;
    const char *path;
    size_t path_len;
    const char *version;
    size_t version_len;
  } request_line;

  struct {
    int status_code;
    const char *status_text;
    size_t status_text_len;
    const char *version;
    size_t version_len;
  } response_line;

  struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
  } header;

  struct {
    const uint8_t *data;
    size_t len;
  } bytes;
} websocket_handshake_token_value_t;

/**
 * @brief WebSocket handshake parser structure
 */
typedef struct {
  const char *data;
  const char *limit;
  const char *cursor;
  const char *start;

  websocket_handshake_state_t state;
  websocket_handshake_mode_t mode;

  // Parsed data
  const char *method_start;
  size_t method_len;
  const char *path_start;
  size_t path_len;
  const char *version_start;
  size_t version_len;

  // Response specific
  int status_code;
  const char *status_text_start;
  size_t status_text_len;

  // Current header being parsed
  const char *header_name_start;
  size_t header_name_len;
  const char *header_value_start;
  size_t header_value_len;

  // WebSocket headers (stored during parsing)
  char *ws_key;           // Sec-WebSocket-Key
  char *ws_version;       // Sec-WebSocket-Version
  char *ws_protocol;      // Sec-WebSocket-Protocol (optional)
  char *ws_accept;        // Sec-WebSocket-Accept (for response)
  char *upgrade;          // Upgrade header
  char *connection;       // Connection header

  // Internal llhttp state (opaque pointers)
  void *http_parser;   // llhttp_t*
  void *http_context;  // llhttp_context_t*
  void *http_settings; // llhttp_settings_t*
} websocket_handshake_parser_t;

/**
 * @brief Initialize WebSocket handshake parser
 * @param parser Parser to initialize
 * @param mode   Parser mode (Request or Response)
 * @param data   Data buffer to parse
 * @param len    Length of data buffer
 */
void websocket_handshake_parser_init(websocket_handshake_parser_t *parser,
                                   websocket_handshake_mode_t mode,
                                   const char *data, size_t len);

/**
 * @brief Scan next token from handshake data
 * @param parser Parser instance
 * @param value Token value output
 * @return Token type
 */
int websocket_handshake_parser_scan(websocket_handshake_parser_t *parser,
                                  websocket_handshake_token_value_t *value);

/**
 * @brief Reset parser for new handshake
 * @param parser Parser to reset
 * @param data New data buffer
 * @param len Length of new data buffer
 */
void websocket_handshake_parser_reset(websocket_handshake_parser_t *parser,
                                    const char *data, size_t len);

/**
 * @brief Destroy parser and free allocated resources
 * @param parser Parser to destroy
 */
void websocket_handshake_parser_destroy(websocket_handshake_parser_t *parser);

/**
 * @brief Check if handshake parsing is complete
 * @param parser Parser instance
 * @return 1 if complete, 0 otherwise
 */
int websocket_handshake_parser_is_complete(const websocket_handshake_parser_t *parser);

/**
 * @brief Get number of bytes consumed by parser
 * @param parser Parser instance
 * @return Bytes consumed
 */
size_t websocket_handshake_parser_bytes_consumed(const websocket_handshake_parser_t *parser);

/**
 * @brief Validate WebSocket handshake request
 * @param parser Parser instance with completed handshake
 * @return 0 on success, negative error code on failure
 */
int websocket_handshake_validate(const websocket_handshake_parser_t *parser);

/**
 * @brief Extract Sec-WebSocket-Key from handshake
 * @param parser Parser instance
 * @param key_buffer Buffer to store key (must be at least 25 bytes)
 * @param buffer_size Size of key buffer
 * @return 0 on success, -1 on failure
 */
int websocket_handshake_get_key(const websocket_handshake_parser_t *parser,
                              char *key_buffer, size_t buffer_size);

/**
 * @brief Check if handshake has required WebSocket headers
 * @param parser Parser instance with completed headers
 * @return 1 if valid WebSocket handshake, 0 otherwise
 */
int websocket_handshake_is_websocket_request(const websocket_handshake_parser_t *parser);

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_HANDSHAKE_PARSER_H
