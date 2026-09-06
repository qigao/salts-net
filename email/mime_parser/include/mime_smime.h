/**
 * @file mime_smime.h
 * @brief S/MIME (Secure MIME) Support using OpenSSL
 *
 * Provides encryption, decryption, signing, and verification for MIME messages.
 * Implements RFC 3851 (S/MIME Version 3.1).
 *
 * Features:
 * - Encrypt/decrypt messages using X.509 certificates
 * - Sign/verify messages using private keys
 * - Support for application/pkcs7-mime content type
 * - Support for multipart/signed messages
 *
 * Requires OpenSSL library.
 */

#ifndef MIME_SMIME_H
#define MIME_SMIME_H

#include "platform.h"
#include <stddef.h>
#include "salts_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Forward declarations ──────────────────────────────────────────── */

typedef struct mime_smime_ctx_s mime_smime_ctx_t;

/* ── S/MIME Operations ─────────────────────────────────────────────── */

typedef enum {
  MIME_SMIME_ENCRYPT,       // Encrypt message
  MIME_SMIME_DECRYPT,       // Decrypt message
  MIME_SMIME_SIGN,          // Sign message
  MIME_SMIME_VERIFY         // Verify signature
} mime_smime_operation_t;

/* ── Error codes ───────────────────────────────────────────────────── */

typedef enum {
  MIME_SMIME_OK = 0,
  MIME_SMIME_ERROR_OPENSSL,
  MIME_SMIME_ERROR_CERT_LOAD,
  MIME_SMIME_ERROR_KEY_LOAD,
  MIME_SMIME_ERROR_ENCRYPT,
  MIME_SMIME_ERROR_DECRYPT,
  MIME_SMIME_ERROR_SIGN,
  MIME_SMIME_ERROR_VERIFY,
  MIME_SMIME_ERROR_MEMORY
} mime_smime_error_t;

/* ── Context management ────────────────────────────────────────────── */

/**
 * Create S/MIME context
 * Must be freed with mime_smime_ctx_free
 */
mime_smime_ctx_t *mime_smime_ctx_create(void);

/**
 * Free S/MIME context
 */
void mime_smime_ctx_free(mime_smime_ctx_t *ctx);

/**
 * Load certificate from PEM file
 * Returns 0 on success, error code on failure
 */
int mime_smime_load_cert(mime_smime_ctx_t *ctx, const char *cert_path);

/**
 * Load certificate from PEM string
 */
int mime_smime_load_cert_mem(mime_smime_ctx_t *ctx,
                                        const char *cert_pem, size_t len);

/**
 * Load private key from PEM file
 * Returns 0 on success, error code on failure
 */
int mime_smime_load_key(mime_smime_ctx_t *ctx,
                                   const char *key_path,
                                   const char *password);

/**
 * Load private key from PEM string
 */
int mime_smime_load_key_mem(mime_smime_ctx_t *ctx,
                                       const char *key_pem, size_t len,
                                       const char *password);

/* ── Encryption/Decryption ─────────────────────────────────────────── */

/**
 * Encrypt MIME message
 * Returns encrypted PKCS7 data (caller must free)
 */
char *mime_smime_encrypt(mime_smime_ctx_t *ctx,
                                    const char *message, size_t message_len,
                                    size_t *output_len,
                                    mime_smime_error_t *error);

/**
 * Decrypt S/MIME message
 * Returns decrypted message (caller must free)
 */
char *mime_smime_decrypt(mime_smime_ctx_t *ctx,
                                    const char *encrypted, size_t encrypted_len,
                                    size_t *output_len,
                                    mime_smime_error_t *error);

/* ── Signing/Verification ──────────────────────────────────────────── */

/**
 * Sign MIME message
 * Returns signed PKCS7 data (caller must free)
 */
char *mime_smime_sign(mime_smime_ctx_t *ctx,
                                 const char *message, size_t message_len,
                                 size_t *output_len,
                                 mime_smime_error_t *error);

/**
 * Verify S/MIME signature
 * Returns 0 if valid, error code if invalid
 * If valid and output is not NULL, returns signed content
 */
int mime_smime_verify(mime_smime_ctx_t *ctx,
                                 const char *signed_data, size_t signed_len,
                                 char **output, size_t *output_len);

/* ── Helpers ───────────────────────────────────────────────────────── */

/**
 * Check if message is S/MIME encrypted
 * Looks for application/pkcs7-mime with smime-type=enveloped-data
 */
int mime_is_smime_encrypted(const char *content_type, size_t len);

/**
 * Check if message is S/MIME signed
 * Looks for application/pkcs7-mime with smime-type=signed-data
 * or multipart/signed with protocol=application/pkcs7-signature
 */
int mime_is_smime_signed(const char *content_type, size_t len);

/**
 * Get last OpenSSL error message
 */
const char *mime_smime_get_error(mime_smime_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MIME_SMIME_H */
