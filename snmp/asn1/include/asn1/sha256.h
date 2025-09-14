/**
 * @file sha256.h
 * @brief SHA-256 Hash Function (RFC 6234)
 *
 * Lightweight SHA-256 implementation for certificate fingerprints.
 */

#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} sha256_ctx_t;

/**
 * Initialize SHA-256 context
 */
void sha256_init(sha256_ctx_t *ctx);

/**
 * Update SHA-256 with data
 */
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);

/**
 * Finalize SHA-256 and output digest
 */
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

/**
 * One-shot SHA-256 hash
 */
void sha256(const uint8_t *data, size_t len, uint8_t digest[SHA256_DIGEST_SIZE]);

#ifdef __cplusplus
}
#endif

#endif // SHA256_H
