#ifndef WEBSOCKET_CRYPTO_H
#define WEBSOCKET_CRYPTO_H

#include <stddef.h>
#include <stdint.h>
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SHA-1 implementation for handshake validation */
typedef struct {
  uint32_t state[5];
  uint32_t count[2];
  uint8_t buffer[64];
} sha1_context_t;

 void sha1_init(sha1_context_t *ctx);
 void sha1_update(sha1_context_t *ctx, const uint8_t *data, size_t len);
 void sha1_final(sha1_context_t *ctx, uint8_t digest[20]);

/* Cryptographically Secure Random */
 int secure_random(uint8_t *buffer, size_t length);

/* UTF-8 Validation */
 int validate_utf8(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_CRYPTO_H
