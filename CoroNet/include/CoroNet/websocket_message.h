#ifndef WEBSOCKET_MESSAGE_H
#define WEBSOCKET_MESSAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// WebSocket opcodes (RFC 6455)
typedef enum {
  WS_OPCODE_CONTINUATION = 0x0,
  WS_OPCODE_TEXT = 0x1,
  WS_OPCODE_BINARY = 0x2,
  WS_OPCODE_CLOSE = 0x8,
  WS_OPCODE_PING = 0x9,
  WS_OPCODE_PONG = 0xA
} websocket_opcode_t;

// Message structure
typedef struct websocket_message_s {
  websocket_opcode_t opcode;
  uint8_t *data;
  size_t length;
  int is_final;
} websocket_message_t;

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_MESSAGE_H
