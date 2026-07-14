// ============================================================================
// WebSocket Common Frame Utilities
// ============================================================================

#include "websocket_message.h"
#include <platform.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// Frame Header Building and Masking
// ============================================================================

// Build frame header into buffer, return header length
size_t websocket_build_frame_header(uint8_t *header, size_t header_max,
                                   websocket_opcode_t opcode, size_t payload_len,
                                   int fin, int masked, uint8_t masking_key[4]) {
  if (header_max < 14) return 0; // Not enough space

  size_t header_len = 0;

  // Byte 1: FIN + opcode
  header[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
  header_len++;

  // Byte 2: MASK + payload length
  if (masked) {
    if (payload_len < 126) {
      header[1] = 0x80 | (uint8_t)payload_len;
      header_len++;
    } else if (payload_len < 65536) {
      header[1] = 0x80 | 126;
      header[2] = (payload_len >> 8) & 0xFF;
      header[3] = payload_len & 0xFF;
      header_len += 3;
    } else {
      header[1] = 0x80 | 127;
      for (int i = 0; i < 8; i++) {
        header[2 + i] = (payload_len >> (56 - i * 8)) & 0xFF;
      }
      header_len += 9;
    }
    // Add masking key
    memcpy(header + header_len, masking_key, 4);
    header_len += 4;
  } else {
    if (payload_len < 126) {
      header[1] = (uint8_t)payload_len;
      header_len++;
    } else if (payload_len < 65536) {
      header[1] = 126;
      header[2] = (payload_len >> 8) & 0xFF;
      header[3] = payload_len & 0xFF;
      header_len += 3;
    } else {
      header[1] = 127;
      for (int i = 0; i < 8; i++) {
        header[2 + i] = (payload_len >> (56 - i * 8)) & 0xFF;
      }
      header_len += 9;
    }
  }

  return header_len;
}

// Generate masking key (client-side)
int websocket_generate_masking_key(uint8_t *key) {
  int rc;

  // RFC 6455: Masking key MUST be unpredictable
  rc = turbo_secure_random(key, 4);
  return rc;
}

// Apply masking to payload
void websocket_apply_mask(uint8_t *data, size_t length, const uint8_t *mask) {
  for (size_t i = 0; i < length; i++) {
    data[i] ^= mask[i % 4];
  }
}
