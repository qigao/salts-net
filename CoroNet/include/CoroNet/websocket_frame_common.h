#ifndef WEBSOCKET_FRAME_COMMON_H
#define WEBSOCKET_FRAME_COMMON_H

#include "websocket_message.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build frame header into buffer, return header length
size_t websocket_build_frame_header(uint8_t *header, size_t header_max, websocket_opcode_t opcode,
                                    size_t payload_len, int fin, int masked,
                                    uint8_t masking_key[4]);

// Generate masking key (client-side)
int websocket_generate_masking_key(uint8_t *key);

// Apply masking to payload
void websocket_apply_mask(uint8_t *data, size_t length, const uint8_t *mask);

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_FRAME_COMMON_H
