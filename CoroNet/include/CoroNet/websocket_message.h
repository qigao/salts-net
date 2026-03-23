#ifndef WEBSOCKET_MESSAGE_H
#define WEBSOCKET_MESSAGE_H

#include "websocket_frame_parser.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
