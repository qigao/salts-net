/**
 * @file x509_generate.h
 * @brief X.509 Certificate Generation (Self-Signed)
 *
 * Generates self-signed X.509 certificates using Ed25519 (RFC 8410).
 * Designed for WebRTC DTLS where temporary certificates are needed.
 */

#ifndef X509_GENERATE_H
#define X509_GENERATE_H

#include "platform.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


// clang-format off
/* ============================================================================
 * Certificate Generation
 * ============================================================================ */

/**
 * Generate Ed25519 key pair for X.509 certificate
 * @param public_key Output: Ed25519 public key (32 bytes)
 * @param private_key Output: Ed25519 private key (32 bytes)
 * @return 0 on success, -1 on error
 */
CXX_C_API int x509_generate_ed25519_keypair(uint8_t public_key[32], uint8_t private_key[32]);

/**
 * Generate self-signed X.509 certificate with Ed25519
 * @param common_name Subject CN (e.g., "WebRTC Peer")
 * @param valid_days Validity period in days
 * @param public_key Ed25519 public key (32 bytes)
 * @param private_key Ed25519 private key (32 bytes, for signing)
 * @param cert_der Output: DER-encoded certificate (caller must free())
 * @param cert_len Output: Certificate length
 * @return 0 on success, -1 on error
 */
CXX_C_API int x509_generate_self_signed_ed25519(
    const char *common_name,
    uint32_t valid_days,
    const uint8_t public_key[32],
    const uint8_t private_key[32],
    uint8_t **cert_der,
    size_t *cert_len
);

/** 
 * @param common_name Subject CN
 * @param valid_days Validity period
 * @param cert_der Output: DER-encoded certificate (caller must free())
 * @param cert_len Output: Certificate length
 * @param private_key Output: Ed25519 private key (32 bytes, for DTLS)
 * @return 0 on success, -1 on error
 */
CXX_C_API int x509_generate_webrtc_cert(
    const char *common_name,
    uint32_t valid_days,
    uint8_t **cert_der,
    size_t *cert_len,
    uint8_t private_key[32]
);

/* ============================================================================
 * Certificate Fingerprint
 * ============================================================================ */

/**
 * Calculate SHA-256 fingerprint of certificate (DER format)
 * @param cert_der DER-encoded certificate
 * @param cert_len Certificate length
 * @param fingerprint Output: SHA-256 hash (32 bytes)
 * @return 0 on success, -1 on error
 */
CXX_C_API int x509_cert_fingerprint_sha256(
    const uint8_t *cert_der,
    size_t cert_len,
    uint8_t fingerprint[32]
);

/**
 * Calculate SHA-256 fingerprint and format as colon-separated hex string
 * @param cert_der DER-encoded certificate
 * @param cert_len Certificate length
 * @param fingerprint_str Output: "AB:CD:EF:..." (96 bytes buffer)
 * @return 0 on success, -1 on error
 */
CXX_C_API int x509_cert_fingerprint_string(
    const uint8_t *cert_der,
    size_t cert_len,
    char fingerprint_str[96]
);

/* ============================================================================
 * PEM Format Conversion (for TLS)
 * ============================================================================ */

/**
 * Convert DER certificate to PEM format
 * @param cert_der DER-encoded certificate
 * @param cert_len Certificate length
 * @param cert_pem Output: PEM string (caller must free())
 * @param pem_len Output: PEM string length (including null terminator)
 * @return 0 on success, -1 on error
 */
CXX_C_API int x509_cert_to_pem(
    const uint8_t *cert_der,
    size_t cert_len,
    char **cert_pem,
    size_t *pem_len
);

/**
 * Convert Ed25519 raw private key to PEM format
 * @param private_key Ed25519 private key (32 bytes)
 * @param key_pem Output: PEM string (caller must free())
 * @param pem_len Output: PEM string length (including null terminator)
 * @return 0 on success, -1 on error
 */
CXX_C_API int x509_privkey_to_pem(
    const uint8_t private_key[32],
    char **key_pem,
    size_t *pem_len
);

/**
 * All-in-one: Generate Ed25519 certificate for TLS (PEM format)
 * Suitable for netcore TLS server/client
 * @param common_name Subject CN
 * @param valid_days Validity period
 * @param cert_pem Output: PEM certificate (caller must free())
 * @param cert_len Output: Certificate PEM length
 * @param key_pem Output: PEM private key (caller must free())
 * @param key_len Output: Private key PEM length
 * @return 0 on success, -1 on error
 */
CXX_C_API int x509_generate_tls_cert_pem(
    const char *common_name,
    uint32_t valid_days,
    char **cert_pem,
    size_t *cert_len,
    char **key_pem,
    size_t *key_len
);

/**
 * Generate ECDSA (P-256) certificate for TLS (PEM format)
 * Better compatibility with Windows/legacy clients than Ed25519.
 * @param common_name Subject CN
 * @param valid_days Validity period
 * @param cert_pem Output: PEM certificate (caller must free())
 * @param cert_len Output: Certificate PEM length
 * @param key_pem Output: PEM private key (caller must free())
 * @param key_len Output: Private key PEM length
 * @return 0 on success, -1 on error
 */
CXX_C_API int x509_generate_tls_cert_pem_ecdsa(
    const char *common_name,
    uint32_t valid_days,
    char **cert_pem,
    size_t *cert_len,
    char **key_pem,
    size_t *key_len
);
// clang-format on
#endif // X509_GENERATE_H
