#ifndef TURBO_CRYPTO_H
#define TURBO_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Vendored implementations are private; this header is the library boundary. */

#define TURBO_CRYPTO_MD5_SIZE 16U
#define TURBO_CRYPTO_SHA256_SIZE 32U
#define TURBO_CRYPTO_SHA256_BLOCK_SIZE 64U
#define TURBO_CRYPTO_SHA256_CONTEXT_SIZE 192U
#define TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE 57U
#define TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE 57U
#define TURBO_CRYPTO_ED448_SIGNATURE_SIZE 114U

#define TURBO_CRYPTO_OK 0
#define TURBO_CRYPTO_EINVAL (-1)
#define TURBO_CRYPTO_ESTATE (-2)
#define TURBO_CRYPTO_EVERIFY (-3)
#define TURBO_CRYPTO_ERANDOM (-4)
#define TURBO_CRYPTO_ECRYPTO (-5)

/**
 * SHA-256 streaming context with implementation-private storage.
 *
 * The context has no heap ownership and is not thread-safe. It must not be
 * copied after initialization because the private state contains self
 * references. One owner must call init, zero or more updates, then final.
 */
typedef union turbo_crypto_sha256_ctx_u {
    void* pointer_alignment;
    uint64_t integer_alignment;
    long double floating_alignment;
    uint8_t bytes[TURBO_CRYPTO_SHA256_CONTEXT_SIZE];
} turbo_crypto_sha256_ctx_t;

/** Time O(len), space O(1). NULL data is valid only when len is zero. */
int turbo_crypto_sha256(const void* data, size_t len,
                        uint8_t out[TURBO_CRYPTO_SHA256_SIZE]);
int turbo_crypto_sha256_init(turbo_crypto_sha256_ctx_t* ctx);
int turbo_crypto_sha256_update(turbo_crypto_sha256_ctx_t* ctx,
                               const void* data, size_t len);
int turbo_crypto_sha256_final(turbo_crypto_sha256_ctx_t* ctx,
                              uint8_t out[TURBO_CRYPTO_SHA256_SIZE]);

/** RFC 2104 HMAC-SHA256. Time O(key_len + data_len), space O(1). */
int turbo_crypto_hmac_sha256(const void* key, size_t key_len,
                             const void* data, size_t data_len,
                             uint8_t out[TURBO_CRYPTO_SHA256_SIZE]);

/** RFC 8018 PBKDF2-HMAC-SHA256. Iterations must be nonzero. */
int turbo_crypto_pbkdf2_hmac_sha256(
    const void* password, size_t password_len,
    const void* salt, size_t salt_len,
    uint32_t iterations, void* out, size_t out_len);

/**
 * RFC 1321 MD5 for protocols that explicitly require MD5.
 * MD5 must not be used for passwords, signatures, or new security designs.
 * Time O(len), space O(1).
 */
int turbo_crypto_md5(const void* data, size_t len,
                     uint8_t out[TURBO_CRYPTO_MD5_SIZE]);

/**
 * Derive an RFC 8032 Ed448 public key from a 57-byte private seed.
 *
 * @param private_key Input seed owned by the caller and left unchanged.
 * @param public_key Output public key owned by the caller.
 * @return TURBO_CRYPTO_OK, TURBO_CRYPTO_EINVAL for NULL arguments, or
 *         TURBO_CRYPTO_ECRYPTO when key derivation fails.
 */
int turbo_crypto_ed448_public_key(
    const uint8_t private_key[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE],
    uint8_t public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE]);

/**
 * Generate an RFC 8032 Ed448 private seed and its public key.
 *
 * The output buffers must be distinct. The caller owns both buffers and must
 * erase the private seed when it is no longer needed.
 *
 * @param private_key Output private seed.
 * @param public_key Output public key.
 * @return TURBO_CRYPTO_OK, TURBO_CRYPTO_EINVAL for invalid buffers,
 *         TURBO_CRYPTO_ERANDOM when the CSPRNG fails, or
 *         TURBO_CRYPTO_ECRYPTO when key derivation fails.
 */
int turbo_crypto_ed448_keygen(
    uint8_t private_key[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE],
    uint8_t public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE]);

/**
 * Create a deterministic pure Ed448 signature as defined by RFC 8032.
 *
 * NULL data is valid only when data_len is zero. Message lengths greater than
 * UINT32_MAX are rejected because libecc's streaming boundary uses u32.
 *
 * @param private_key Input 57-byte private seed.
 * @param data Message bytes.
 * @param data_len Message length in bytes.
 * @param signature Output 114-byte signature.
 * @return TURBO_CRYPTO_OK, TURBO_CRYPTO_EINVAL for invalid input, or
 *         TURBO_CRYPTO_ECRYPTO when signing fails.
 */
int turbo_crypto_ed448_sign(
    const uint8_t private_key[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE],
    const void* data, size_t data_len,
    uint8_t signature[TURBO_CRYPTO_ED448_SIGNATURE_SIZE]);

/**
 * Verify a pure RFC 8032 Ed448 signature.
 *
 * @param public_key Input 57-byte canonical public key.
 * @param data Message bytes; NULL is valid only when data_len is zero.
 * @param data_len Message length in bytes, at most UINT32_MAX.
 * @param signature Input 114-byte signature.
 * @return TURBO_CRYPTO_OK, TURBO_CRYPTO_EINVAL for invalid arguments, or
 *         TURBO_CRYPTO_EVERIFY for an invalid key or signature. An internal
 *         curve initialization failure returns TURBO_CRYPTO_ECRYPTO.
 */
int turbo_crypto_ed448_verify(
    const uint8_t public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE],
    const void* data, size_t data_len,
    const uint8_t signature[TURBO_CRYPTO_ED448_SIGNATURE_SIZE]);

/** Fill output from the operating-system CSPRNG. */
int turbo_crypto_random(void* out, size_t len);

/** Constant-time byte comparison. */
int turbo_crypto_verify(const void* expected, const void* actual, size_t len);

/** Erase sensitive memory through Monocypher. */
void turbo_crypto_wipe(void* secret, size_t len);

#ifdef __cplusplus
}
#endif

#endif
