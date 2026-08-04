#ifndef TURBO_CORO_WEBSOCKET_H
#define TURBO_CORO_WEBSOCKET_H

/**
 * @file turbo_coro_websocket.h
 * @brief Transport-independent WebSocket session for CoroNet adapters.
 *
 * The session starts after the HTTP handshake. HTTP/1.1 Upgrade, HTTP/2
 * extended CONNECT, and HTTP/3 extended CONNECT are transport concerns; this
 * API owns RFC 6455 frame/message state after a stream has been established.
 */

#include <stddef.h>
#include <stdint.h>

#include "turbo_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coro_websocket_s coro_websocket_t;

typedef enum coro_websocket_role_e {
  CORO_WEBSOCKET_CLIENT = 0,
  CORO_WEBSOCKET_SERVER = 1
} coro_websocket_role_t;

typedef enum coro_websocket_transport_e {
  CORO_WEBSOCKET_TRANSPORT_HTTP1 = 1,
  CORO_WEBSOCKET_TRANSPORT_HTTP2 = 2,
  CORO_WEBSOCKET_TRANSPORT_HTTP3 = 3
} coro_websocket_transport_t;

typedef enum coro_websocket_opcode_e {
  CORO_WEBSOCKET_CONTINUATION = 0x0,
  CORO_WEBSOCKET_TEXT = 0x1,
  CORO_WEBSOCKET_BINARY = 0x2,
  CORO_WEBSOCKET_CLOSE = 0x8,
  CORO_WEBSOCKET_PING = 0x9,
  CORO_WEBSOCKET_PONG = 0xA
} coro_websocket_opcode_t;

typedef enum coro_websocket_close_code_e {
  CORO_WEBSOCKET_CLOSE_NORMAL = 1000,
  CORO_WEBSOCKET_CLOSE_GOING_AWAY = 1001,
  CORO_WEBSOCKET_CLOSE_PROTOCOL_ERROR = 1002,
  CORO_WEBSOCKET_CLOSE_UNSUPPORTED_DATA = 1003,
  CORO_WEBSOCKET_CLOSE_INVALID_PAYLOAD = 1007,
  CORO_WEBSOCKET_CLOSE_POLICY_VIOLATION = 1008,
  CORO_WEBSOCKET_CLOSE_MESSAGE_TOO_BIG = 1009,
  CORO_WEBSOCKET_CLOSE_MANDATORY_EXTENSION = 1010,
  CORO_WEBSOCKET_CLOSE_INTERNAL_ERROR = 1011
} coro_websocket_close_code_t;

/**
 * @brief Complete message callback.
 *
 * @p data is borrowed and valid only until the callback returns. The callback
 * runs on the session owner lane and must not retain the pointer or re-enter
 * the same session. It may call coro_websocket_destroy(); destruction is
 * deferred until the active API call returns. Do not call other session APIs
 * after requesting destruction from a callback.
 *
 * @return 0 to continue; non-zero rejects the message, sends a 1008 close
 * frame, and closes the session. The feed call returns TURBO_ECANCELED unless
 * sending the close frame fails first.
 */
typedef int (*coro_websocket_message_cb)(
    coro_websocket_t *websocket, coro_websocket_opcode_t opcode,
    const void *data, size_t len, void *user_data);

/**
 * @brief Transport callback for one complete, already encoded WebSocket frame.
 *
 * The callback must synchronously copy @p data before returning; the pointer
 * is borrowed and cannot be retained after the callback returns.
 * When @p end_stream is non-zero, the transport must also finish its logical
 * HTTP stream after the frame is accepted. The callback is called only on the
 * session owner lane. It may call coro_websocket_destroy(); destruction is
 * deferred until the active API call returns.
 */
typedef int (*coro_websocket_send_cb)(void *user_data, const uint8_t *data,
                                      size_t len, int end_stream);

typedef struct coro_websocket_options_s {
  size_t size;
  coro_websocket_role_t role;
  coro_websocket_transport_t transport;
  size_t max_message_size;
  coro_websocket_message_cb on_message;
  void *user_data;
} coro_websocket_options_t;

typedef struct coro_websocket_transport_ops_s {
  size_t size;
  coro_websocket_send_cb send;
  void *user_data;
} coro_websocket_transport_ops_t;

#define CORO_WEBSOCKET_DEFAULT_MAX_MESSAGE_SIZE (16U * 1024U * 1024U)
#define CORO_WEBSOCKET_OPTIONS_V1_SIZE \
  (offsetof(coro_websocket_options_t, user_data) + \
   sizeof(((coro_websocket_options_t *)0)->user_data))
#define CORO_WEBSOCKET_TRANSPORT_OPS_V1_SIZE \
  (offsetof(coro_websocket_transport_ops_t, user_data) + \
   sizeof(((coro_websocket_transport_ops_t *)0)->user_data))

/** Initialize options without writing past the caller-provided size. */
CXX_C_API int coro_websocket_options_init(coro_websocket_options_t *options,
                                          size_t options_size);

/**
 * Create an open WebSocket session after the HTTP handshake.
 *
 * The options and transport structures are copied. The callback user_data
 * pointers remain caller-owned and are borrowed until destroy().
 */
CXX_C_API int coro_websocket_create(
    const coro_websocket_options_t *options,
    const coro_websocket_transport_ops_t *transport,
    coro_websocket_t **out_websocket);

CXX_C_API void coro_websocket_destroy(coro_websocket_t *websocket);

/** Feed one borrowed HTTP stream DATA payload into the frame parser. */
CXX_C_API int coro_websocket_feed(coro_websocket_t *websocket,
                                  const void *data, size_t len);

/**
 * Send one unfragmented text, binary, ping, or pong frame.
 * Text payloads must be valid UTF-8.
 */
CXX_C_API int coro_websocket_send(coro_websocket_t *websocket,
                                  coro_websocket_opcode_t opcode,
                                  const void *data, size_t len);

/**
 * Send a close frame and mark the logical stream closed.
 * @p reason must be NULL or a NUL-terminated UTF-8 string of at most 123 bytes.
 */
CXX_C_API int coro_websocket_close(coro_websocket_t *websocket,
                                   uint16_t code, const char *reason);

/** Mark the session closed after the underlying stream was reset/closed. */
CXX_C_API void coro_websocket_mark_closed(coro_websocket_t *websocket);

/** Return non-zero while the session can receive or send WebSocket frames. */
CXX_C_API int coro_websocket_is_open(const coro_websocket_t *websocket);

/** Return non-zero if an input DATA stream ended between complete frames. */
CXX_C_API int coro_websocket_input_complete(
    const coro_websocket_t *websocket);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CORO_WEBSOCKET_H */
