/**
 * @file x509_generate.c
 * @brief X.509 Certificate Generation Implementation
 */

#include "x509_generate.h"
#include "x509_cert.h"
#include <asn1/asn1_der_compat.h>
#include "sha256.h"
#include "crypto_random.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* Use OpenSSL for Ed25519 key generation and signing (for WebRTC DTLS compatibility) */
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/ec.h>
#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>
/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static uint64_t get_random_serial(void) {
    // Generate cryptographically secure random serial number
    uint8_t bytes[8];
    if (crypto_random_bytes(bytes, 8) < 0) {
        // Fallback to time-based (should not happen)
        return (uint64_t)time(NULL);
    }

    uint64_t serial = 0;
    for (int i = 0; i < 8; i++) {
        serial = (serial << 8) | bytes[i];
    }

    // Ensure positive (clear MSB)
    serial &= 0x7FFFFFFFFFFFFFFF;

    return serial;
}

static asn1_value_t *create_ed25519_algorithm_id(void) {
    // AlgorithmIdentifier for Ed25519: SEQUENCE { OID }
    asn1_value_t *alg_seq = asn1_create_sequence();
    asn1_sequence_add_child(alg_seq, asn1_create_oid_from_string(X509_OID_ED25519));
    return alg_seq;
}

static asn1_value_t *create_name(const char *common_name) {
    // Name ::= SEQUENCE OF RDN
    // RDN ::= SET OF AttributeTypeAndValue
    // For simplicity: just CN
    asn1_value_t *name = asn1_create_sequence();

    asn1_value_t *rdn = asn1_create_set();
    asn1_value_t *attr_seq = asn1_create_sequence();
    asn1_sequence_add_child(attr_seq, asn1_create_oid_from_string(X509_OID_AT_CN));
    asn1_sequence_add_child(attr_seq, asn1_create_utf8_string(common_name));
    asn1_set_add_child(rdn, attr_seq);

    asn1_sequence_add_child(name, rdn);
    return name;
}

static asn1_value_t *create_validity(uint32_t valid_days) {
    // Validity ::= SEQUENCE { notBefore UTCTime, notAfter UTCTime }
    time_t now = time(NULL);
    time_t future = now + (valid_days * 24 * 3600);

    struct tm *tm_now = gmtime(&now);
    struct tm *tm_future = gmtime(&future);

    char not_before[14], not_after[14];
    stbsp_snprintf(not_before, sizeof(not_before), "%02d%02d%02d%02d%02d%02dZ",
             tm_now->tm_year % 100, tm_now->tm_mon + 1, tm_now->tm_mday,
             tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);

    stbsp_snprintf(not_after, sizeof(not_after), "%02d%02d%02d%02d%02d%02dZ",
             tm_future->tm_year % 100, tm_future->tm_mon + 1, tm_future->tm_mday,
             tm_future->tm_hour, tm_future->tm_min, tm_future->tm_sec);

    asn1_value_t *validity = asn1_create_sequence();
    asn1_sequence_add_child(validity, asn1_create_utc_time(not_before));
    asn1_sequence_add_child(validity, asn1_create_utc_time(not_after));

    return validity;
}

static asn1_value_t *create_ed25519_pubkey_info(const uint8_t public_key[32]) {
    // SubjectPublicKeyInfo ::= SEQUENCE {
    //   algorithm AlgorithmIdentifier,
    //   subjectPublicKey BIT STRING }

    asn1_value_t *pubkey_info = asn1_create_sequence();
    asn1_sequence_add_child(pubkey_info, create_ed25519_algorithm_id());

    // Ed25519 public key as BIT STRING (no unused bits)
    asn1_value_t *pubkey_bits = asn1_create_bit_string(public_key, 32, 0);
    asn1_sequence_add_child(pubkey_info, pubkey_bits);

    return pubkey_info;
}

static asn1_value_t *create_ecdsa_algorithm_id(void) {
    // AlgorithmIdentifier for ECDSA: SEQUENCE { OID (ecdsa-with-SHA256) }
    // Note: parameters typically ABSENT for ecdsa-with-SHA256 in signatureAlgorithm
    // BUT for SubjectPublicKeyInfo it is id-ecPublicKey with parameters (curve OID)
    asn1_value_t *alg_seq = asn1_create_sequence();
    asn1_sequence_add_child(alg_seq, asn1_create_oid_from_string(X509_OID_SHA256_ECDSA));
    return alg_seq;
}

static asn1_value_t *create_ecdsa_pubkey_info(const uint8_t *pub_bytes, size_t pub_len) {
    // SubjectPublicKeyInfo ::= SEQUENCE {
    //   algorithm AlgorithmIdentifier,
    //   subjectPublicKey BIT STRING }
    // AlgorithmIdentifier for EC:
    //   algorithm: id-ecPublicKey (1.2.840.10045.2.1)
    //   parameters: non-optional, NamedCurve OID (1.2.840.10045.3.1.7 for P-256)

    asn1_value_t *pubkey_info = asn1_create_sequence();

    // AlgorithmIdentifier
    asn1_value_t *alg_seq = asn1_create_sequence();
    asn1_sequence_add_child(alg_seq, asn1_create_oid_from_string(X509_OID_EC_PUBKEY));
    asn1_sequence_add_child(alg_seq, asn1_create_oid_from_string(X509_OID_SECP256R1));
    asn1_sequence_add_child(pubkey_info, alg_seq);

    // SubjectPublicKey: BIT STRING containing the EC point (04 | x | y)
    asn1_value_t *pubkey_bits = asn1_create_bit_string(pub_bytes, pub_len, 0);
    asn1_sequence_add_child(pubkey_info, pubkey_bits);

    return pubkey_info;
}

static asn1_value_t *create_extension(const char *oid_str, bool critical, asn1_value_t *value) {
    // Extension ::= SEQUENCE {
    //   extnID     OBJECT IDENTIFIER,
    //   critical   BOOLEAN DEFAULT FALSE,
    //   extnValue  OCTET STRING }

    // Encode value to OCTET STRING
    uint8_t buf[1024];
    size_t len = sizeof(buf);
    if (asn1_der_encode_legacy(value, buf, &len) < 0) {
        asn1_free(value);
        return NULL;
    }
    asn1_free(value); // Value is now encoded

    asn1_value_t *ext = asn1_create_sequence();
    asn1_sequence_add_child(ext, asn1_create_oid_from_string(oid_str));
    if (critical) {
        asn1_sequence_add_child(ext, asn1_create_boolean(true));
    }
    asn1_sequence_add_child(ext, asn1_create_octet_string(buf, len));

    return ext;
}

static asn1_value_t *create_basic_constraints(void) {
    // BasicConstraints ::= SEQUENCE {
    //   cA                      BOOLEAN DEFAULT FALSE,
    //   pathLenConstraint       INTEGER OPTIONAL }
    asn1_value_t *bc = asn1_create_sequence();
    asn1_sequence_add_child(bc, asn1_create_boolean(true)); // CA=TRUE
    return create_extension(X509_OID_EXT_BASIC_CONSTRAINTS, true, bc);
}

static asn1_value_t *create_key_usage(void) {
    // KeyUsage ::= BIT STRING {
    //   digitalSignature (0),
    //   keyCertSign (5),
    //   cRLSign (6) }
    // Use: DigitalSignature | KeyCertSign (0x80 | 0x04 = 0x84) -> 10000100
    // But bit string is big endian bits?
    // Bit 0 is first bit.
    // 0: DigitalSignature
    // 5: KeyCertSign
    // 6: CRLSign
    // Byte 0: 01234567
    // Bits 0,5,6 -> 10000110 = 0x86
    uint8_t bits = 0x86; // DigitalSignature | KeyCertSign | CRLSign
    asn1_value_t *ku = asn1_create_bit_string(&bits, 1, 0); // 1 unused bit? No, 8 bits used?
    // KeyUsage is usually 9 bits (0..8)
    // If we use 1 byte, we cover 0..7
    // Unused bits = 0 means used all 8 bits of the byte.
    return create_extension(X509_OID_EXT_KEY_USAGE, true, ku);
}

static asn1_value_t *create_ext_key_usage(void) {
    // ExtKeyUsageSyntax ::= SEQUENCE SIZE (1..MAX) OF KeyPurposeId
    asn1_value_t *eku = asn1_create_sequence();
    asn1_sequence_add_child(eku, asn1_create_oid_from_string(X509_OID_KP_SERVER_AUTH));
    asn1_sequence_add_child(eku, asn1_create_oid_from_string(X509_OID_KP_CLIENT_AUTH));
    return create_extension(X509_OID_EXT_EXTENDED_KEY_USAGE, false, eku);
}

static asn1_value_t *create_san(const char *common_name) {
    // SubjectAltName ::= GeneralNames
    // GeneralNames ::= SEQUENCE SIZE (1..MAX) OF GeneralName
    // GeneralName ::= CHOICE {
    //   dNSName [2] IA5String,
    //   iPAddress [7] OCTET STRING, ... }

    asn1_value_t *san = asn1_create_sequence();

    // Add DNS:common_name
    asn1_value_t *dns = asn1_create_ia5_string(common_name);
    // Wrap in [2] IMPLICIT
    dns->tag = 0x82; // Context-specific [2] Primitive
    dns->tag_class = 2; // Context-specific
    dns->constructed = 0; // Primitive
    dns->tag_number = 2; // [2]
    asn1_sequence_add_child(san, dns);

    // If 'localhost', also add IP:127.0.0.1
    // (Simple check for now)
    if (strcmp(common_name, "localhost") == 0) {
        uint8_t ip[] = {127, 0, 0, 1};
        asn1_value_t *ip_val = asn1_create_octet_string(ip, 4);
        // Wrap in [7] IMPLICIT
        ip_val->tag = 0x87; // Context-specific [7] Primitive
        ip_val->tag_class = 2; // Context-specific
        ip_val->constructed = 0; // Primitive
        ip_val->tag_number = 7; // [7]
        asn1_sequence_add_child(san, ip_val);
    }

    return create_extension(X509_OID_EXT_SUBJECT_ALT_NAME, false, san);
}

static asn1_value_t *create_extensions(const char *common_name) {
    // Extensions ::= SEQUENCE SIZE (1..MAX) OF Extension
    asn1_value_t *exts = asn1_create_sequence();

    asn1_sequence_add_child(exts, create_basic_constraints());
    asn1_sequence_add_child(exts, create_key_usage());
    asn1_sequence_add_child(exts, create_ext_key_usage());
    asn1_sequence_add_child(exts, create_san(common_name));

    // Wrap in [3] EXPLICIT
    // But X.509 v3 says:
    // extensions [3] EXPLICIT Extensions OPTIONAL
    // So we return the SEQUENCE Of Extensions, and wrapper handles the tag [3]
    return exts;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int x509_generate_ed25519_keypair(uint8_t public_key[32], uint8_t private_key[32]) {
    if (!public_key || !private_key) return -1;

    // Generate cryptographically secure random seed (32 bytes)
    uint8_t seed[32];
    if (crypto_random_bytes(seed, 32) < 0) {
        return -1;
    }

    // Use OpenSSL to generate Ed25519 keypair from seed
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    if (!pkey) {
        memset(seed, 0, sizeof(seed));
        return -1;
    }

    // Extract public key (32 bytes)
    size_t pub_len = 32;
    if (EVP_PKEY_get_raw_public_key(pkey, public_key, &pub_len) != 1 || pub_len != 32) {
        EVP_PKEY_free(pkey);
        memset(seed, 0, sizeof(seed));
        return -1;
    }

    // Return the original seed as private_key (OpenSSL format)
    memcpy(private_key, seed, 32);

    // Clear sensitive data
    memset(seed, 0, sizeof(seed));
    EVP_PKEY_free(pkey);

    return 0;
}

int x509_generate_self_signed_ed25519(
    const char *common_name,
    uint32_t valid_days,
    const uint8_t public_key[32],
    const uint8_t private_key[32],
    uint8_t **cert_der,
    size_t *cert_len
) {
    if (!common_name || !public_key || !private_key || !cert_der || !cert_len) {
        return -1;
    }

    // Build TBSCertificate
    asn1_value_t *tbs = asn1_create_sequence();

    // Version [0] EXPLICIT v3 (2)
    asn1_value_t *version_ctx = asn1_create_sequence();
    version_ctx->tag = 0xA0;  // [0] EXPLICIT
    version_ctx->tag_class = 2; // Context-specific
    version_ctx->constructed = 1; // Constructed
    version_ctx->tag_number = 0; // [0]
    asn1_sequence_add_child(version_ctx, asn1_create_integer(2));
    asn1_sequence_add_child(tbs, version_ctx);

    // Serial Number
    asn1_sequence_add_child(tbs, asn1_create_integer(get_random_serial()));

    // Signature Algorithm
    asn1_sequence_add_child(tbs, create_ed25519_algorithm_id());

    // Issuer (same as subject for self-signed)
    asn1_sequence_add_child(tbs, create_name(common_name));

    // Validity
    asn1_sequence_add_child(tbs, create_validity(valid_days));

    // Subject
    asn1_sequence_add_child(tbs, create_name(common_name));

    // SubjectPublicKeyInfo
    asn1_sequence_add_child(tbs, create_ed25519_pubkey_info(public_key));

    // Extensions [3] EXPLICIT
    asn1_value_t *exts_seq = create_extensions(common_name);
    // X.509 v3 Extensions: [3] EXPLICIT Extensions
    // Extensions ::= SEQUENCE OF Extension
    // In DER, EXPLICIT tagging means the tag replaces the original tag if IMPLICIT,
    // or wraps it if EXPLICIT. 
    // Wait, [3] EXPLICIT means:
    // val = Extension...
    // wrap = SEQUENCE { val }  <-- this is the "Extensions" type
    // explicit = [3] { wrap }
    //
    // So we have `exts_seq` which is the SEQUENCE of Extensions.
    // We need to wrap it in a container with tag [3].
    
    asn1_value_t *exts_wrapper = asn1_create_sequence();
    exts_wrapper->tag = 0xA3; // [3] Context-Specific Constructed
    exts_wrapper->tag_class = 2; // Context-specific
    exts_wrapper->constructed = 1; // Constructed
    exts_wrapper->tag_number = 3; // [3]
    asn1_sequence_add_child(exts_wrapper, exts_seq);
    
    asn1_sequence_add_child(tbs, exts_wrapper);

    // Encode TBS to DER
    uint8_t tbs_der[2048];
    size_t tbs_len = sizeof(tbs_der);
    if (asn1_der_encode(tbs, tbs_der, &tbs_len) < 0) {
        asn1_free(tbs);
        return -1;
    }

    // Sign TBS with Ed25519 using OpenSSL
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private_key, 32);
    if (!pkey) {
        asn1_free(tbs);
        return -1;
    }

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        EVP_PKEY_free(pkey);
        asn1_free(tbs);
        return -1;
    }

    // Initialize signing context (Ed25519 doesn't use a digest)
    if (EVP_DigestSignInit(mdctx, NULL, NULL, NULL, pkey) != 1) {
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        asn1_free(tbs);
        return -1;
    }

    // Sign the TBS certificate
    uint8_t signature[64];
    size_t sig_len = sizeof(signature);
    if (EVP_DigestSign(mdctx, signature, &sig_len, tbs_der, tbs_len) != 1 || sig_len != 64) {
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        asn1_free(tbs);
        return -1;
    }

    // Clean up OpenSSL resources
    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);

    // Build complete Certificate
    asn1_value_t *cert = asn1_create_sequence();
    asn1_sequence_add_child(cert, tbs);
    asn1_sequence_add_child(cert, create_ed25519_algorithm_id());
    asn1_sequence_add_child(cert, asn1_create_bit_string(signature, 64, 0));

    // Encode certificate to DER
    uint8_t cert_buf[4096];
    size_t cert_buf_len = sizeof(cert_buf);
    if (asn1_der_encode(cert, cert_buf, &cert_buf_len) < 0) {
        asn1_free(cert);
        return -1;
    }

    // Allocate output
    *cert_der = malloc(cert_buf_len);
    if (!*cert_der) {
        asn1_free(cert);
        return -1;
    }

    memcpy(*cert_der, cert_buf, cert_buf_len);
    *cert_len = cert_buf_len;

    asn1_free(cert);
    return 0;
}

int x509_generate_webrtc_cert(
    const char *common_name,
    uint32_t valid_days,
    uint8_t **cert_der,
    size_t *cert_len,
    uint8_t private_key[32]
) {
    uint8_t public_key[32];

    // Generate keypair
    if (x509_generate_ed25519_keypair(public_key, private_key) < 0) {
        return -1;
    }

    // Generate certificate
    return x509_generate_self_signed_ed25519(
        common_name, valid_days,
        public_key, private_key,
        cert_der, cert_len
    );
}

/* ============================================================================
 * Certificate Fingerprint
 * ============================================================================ */

int x509_cert_fingerprint_sha256(
    const uint8_t *cert_der,
    size_t cert_len,
    uint8_t fingerprint[32]
) {
    if (!cert_der || cert_len == 0 || !fingerprint) {
        return -1;
    }

    // Calculate SHA-256 hash of entire DER-encoded certificate
    sha256(cert_der, cert_len, fingerprint);
    return 0;
}

int x509_cert_fingerprint_string(
    const uint8_t *cert_der,
    size_t cert_len,
    char fingerprint_str[96]
) {
    if (!cert_der || cert_len == 0 || !fingerprint_str) {
        return -1;
    }

    // Calculate fingerprint
    uint8_t fingerprint[32];
    if (x509_cert_fingerprint_sha256(cert_der, cert_len, fingerprint) < 0) {
        return -1;
    }

    // Format as colon-separated hex string (SHA-256: AB:CD:EF:...)
    static const char hex[] = "0123456789ABCDEF";
    char *p = fingerprint_str;

    for (int i = 0; i < 32; i++) {
        *p++ = hex[(fingerprint[i] >> 4) & 0x0F];
        *p++ = hex[fingerprint[i] & 0x0F];
        if (i < 31) {
            *p++ = ':';
        }
    }
    *p = '\0';

    return 0;
}

/* ============================================================================
 * PEM Format Conversion
 * ============================================================================ */

int x509_cert_to_pem(
    const uint8_t *cert_der,
    size_t cert_len,
    char **cert_pem,
    size_t *pem_len
) {
    if (!cert_der || cert_len == 0 || !cert_pem || !pem_len) {
        return -1;
    }

    // Convert DER to X509 structure
    const uint8_t *p = cert_der;
    X509 *x509 = d2i_X509(NULL, &p, (long)cert_len);
    if (!x509) {
        return -1;
    }

    // Create memory BIO for PEM output
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) {
        X509_free(x509);
        return -1;
    }

    // Write certificate in PEM format
    if (PEM_write_bio_X509(bio, x509) != 1) {
        BIO_free(bio);
        X509_free(x509);
        return -1;
    }

    // Extract PEM string from BIO
    BUF_MEM *mem;
    BIO_get_mem_ptr(bio, &mem);

    // Allocate output buffer (include null terminator)
    *cert_pem = (char *)malloc(mem->length + 1);
    if (!*cert_pem) {
        BIO_free(bio);
        X509_free(x509);
        return -1;
    }

    memcpy(*cert_pem, mem->data, mem->length);
    (*cert_pem)[mem->length] = '\0';
    *pem_len = mem->length + 1;

    // Cleanup
    BIO_free(bio);
    X509_free(x509);

    return 0;
}

int x509_privkey_to_pem(
    const uint8_t private_key[32],
    char **key_pem,
    size_t *pem_len
) {
    if (!private_key || !key_pem || !pem_len) {
        return -1;
    }

    // Create EVP_PKEY from raw Ed25519 key
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private_key, 32);
    if (!pkey) {
        return -1;
    }

    // Create memory BIO for PEM output
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) {
        EVP_PKEY_free(pkey);
        return -1;
    }

    // Write private key in PEM format (no encryption)
    if (PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL) != 1) {
        BIO_free(bio);
        EVP_PKEY_free(pkey);
        return -1;
    }

    // Extract PEM string from BIO
    BUF_MEM *mem;
    BIO_get_mem_ptr(bio, &mem);

    // Allocate output buffer (include null terminator)
    *key_pem = (char *)malloc(mem->length + 1);
    if (!*key_pem) {
        BIO_free(bio);
        EVP_PKEY_free(pkey);
        return -1;
    }

    memcpy(*key_pem, mem->data, mem->length);
    (*key_pem)[mem->length] = '\0';
    *pem_len = mem->length + 1;

    // Cleanup
    BIO_free(bio);
    EVP_PKEY_free(pkey);

    return 0;
}

int x509_generate_tls_cert_pem(
    const char *common_name,
    uint32_t valid_days,
    char **cert_pem,
    size_t *cert_len,
    char **key_pem,
    size_t *key_len
) {
    if (!common_name || !cert_pem || !cert_len || !key_pem || !key_len) {
        return -1;
    }

    // Generate certificate in DER format
    uint8_t *cert_der = NULL;
    size_t cert_der_len = 0;
    uint8_t private_key[32];

    if (x509_generate_webrtc_cert(common_name, valid_days, &cert_der, &cert_der_len, private_key) != 0) {
        return -1;
    }

    // Convert DER to PEM
    if (x509_cert_to_pem(cert_der, cert_der_len, cert_pem, cert_len) != 0) {
        free(cert_der);
        memset(private_key, 0, sizeof(private_key));
        return -1;
    }

    // Convert private key to PEM
    if (x509_privkey_to_pem(private_key, key_pem, key_len) != 0) {
        free(cert_der);
        free(*cert_pem);
        memset(private_key, 0, sizeof(private_key));
        return -1;
    }

    // Cleanup
    free(cert_der);
    memset(private_key, 0, sizeof(private_key));

    return 0;
}

/* ============================================================================
 * ECDSA P-256 Implementation
 * ============================================================================ */

static int x509_generate_ecdsa_keypair_internal(EVP_PKEY **pkey_out, uint8_t *pub_bytes, size_t *pub_len) {
    if (!pkey_out || !pub_bytes || !pub_len) {
        return -1;
    }

    // Create ECDSA P-256 key
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!pctx) {
        return -1;
    }

    if (EVP_PKEY_keygen_init(pctx) != 1) {
        EVP_PKEY_CTX_free(pctx);
        return -1;
    }

    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) != 1) {
        EVP_PKEY_CTX_free(pctx);
        return -1;
    }

    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen(pctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(pctx);
        return -1;
    }
    EVP_PKEY_CTX_free(pctx);

    // Extract public key using EC_KEY API
    EC_KEY *ec_key = EVP_PKEY_get1_EC_KEY(pkey);
    if (!ec_key) {
        EVP_PKEY_free(pkey);
        return -1;
    }

    const EC_POINT *pub_point = EC_KEY_get0_public_key(ec_key);
    const EC_GROUP *group = EC_KEY_get0_group(ec_key);

    // Convert EC_POINT to uncompressed octet string (04 | x | y)
    *pub_len = EC_POINT_point2oct(group, pub_point, POINT_CONVERSION_UNCOMPRESSED, pub_bytes, 65, NULL);

    EC_KEY_free(ec_key);

    if (*pub_len != 65) {
        EVP_PKEY_free(pkey);
        return -1;
    }

    *pkey_out = pkey;
    return 0;
}

static int x509_generate_self_signed_ecdsa(
    const char *common_name,
    uint32_t valid_days,
    const uint8_t *pub_bytes,
    size_t pub_len,
    EVP_PKEY *pkey,
    uint8_t **cert_der,
    size_t *cert_len
) {
    if (!common_name || !pub_bytes || !pkey || !cert_der || !cert_len) {
        return -1;
    }

    // Build TBSCertificate
    asn1_value_t *tbs = asn1_create_sequence();

    // Version [0] EXPLICIT v3 (2)
    asn1_value_t *version_ctx = asn1_create_sequence();
    version_ctx->tag = 0xA0;
    version_ctx->tag_class = 2; // Context-specific
    version_ctx->constructed = 1; // Constructed
    version_ctx->tag_number = 0; // [0]
    asn1_sequence_add_child(version_ctx, asn1_create_integer(2));
    asn1_sequence_add_child(tbs, version_ctx);

    // Serial Number
    asn1_sequence_add_child(tbs, asn1_create_integer(get_random_serial()));

    // Signature Algorithm
    asn1_sequence_add_child(tbs, create_ecdsa_algorithm_id());

    // Issuer
    asn1_sequence_add_child(tbs, create_name(common_name));

    // Validity
    asn1_sequence_add_child(tbs, create_validity(valid_days));

    // Subject
    asn1_sequence_add_child(tbs, create_name(common_name));

    // SubjectPublicKeyInfo
    asn1_sequence_add_child(tbs, create_ecdsa_pubkey_info(pub_bytes, pub_len));

    // Extensions [3] EXPLICIT
    asn1_value_t *exts_seq = create_extensions(common_name);
    asn1_value_t *exts_wrapper = asn1_create_sequence();
    exts_wrapper->tag = 0xA3;
    exts_wrapper->tag_class = 2; // Context-specific
    exts_wrapper->constructed = 1; // Constructed
    exts_wrapper->tag_number = 3; // [3]
    asn1_sequence_add_child(exts_wrapper, exts_seq);
    asn1_sequence_add_child(tbs, exts_wrapper);

    // Encode TBS to DER
    uint8_t tbs_der[2048];
    size_t tbs_len = sizeof(tbs_der);
    if (asn1_der_encode(tbs, tbs_der, &tbs_len) < 0) {
        asn1_free(tbs);
        return -1;
    }

    // Sign TBS with ECDSA
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        asn1_free(tbs);
        return -1;
    }

    if (EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, pkey) != 1) {
        EVP_MD_CTX_free(mdctx);
        asn1_free(tbs);
        return -1;
    }

    // Get signature size
    size_t sig_len = 0;
    if (EVP_DigestSign(mdctx, NULL, &sig_len, tbs_der, tbs_len) != 1) {
        EVP_MD_CTX_free(mdctx);
        asn1_free(tbs);
        return -1;
    }

    // Allocate and sign
    uint8_t *signature = malloc(sig_len);
    if (!signature) {
        EVP_MD_CTX_free(mdctx);
        asn1_free(tbs);
        return -1;
    }

    if (EVP_DigestSign(mdctx, signature, &sig_len, tbs_der, tbs_len) != 1) {
        free(signature);
        EVP_MD_CTX_free(mdctx);
        asn1_free(tbs);
        return -1;
    }

    EVP_MD_CTX_free(mdctx);

    // Build complete Certificate
    asn1_value_t *cert = asn1_create_sequence();
    asn1_sequence_add_child(cert, tbs);
    asn1_sequence_add_child(cert, create_ecdsa_algorithm_id());
    asn1_sequence_add_child(cert, asn1_create_bit_string(signature, sig_len, 0));

    free(signature);

    // Encode certificate to DER
    uint8_t cert_buf[4096];
    size_t cert_buf_len = sizeof(cert_buf);
    if (asn1_der_encode(cert, cert_buf, &cert_buf_len) < 0) {
        asn1_free(cert);
        return -1;
    }

    // Allocate output
    *cert_der = malloc(cert_buf_len);
    if (!*cert_der) {
        asn1_free(cert);
        return -1;
    }

    memcpy(*cert_der, cert_buf, cert_buf_len);
    *cert_len = cert_buf_len;

    asn1_free(cert);
    return 0;
}

static int x509_privkey_ecdsa_to_pem(EVP_PKEY *pkey, char **key_pem, size_t *pem_len) {
    if (!pkey || !key_pem || !pem_len) {
        return -1;
    }

    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) {
        return -1;
    }

    if (PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL) != 1) {
        BIO_free(bio);
        return -1;
    }

    BUF_MEM *mem;
    BIO_get_mem_ptr(bio, &mem);

    *key_pem = (char *)malloc(mem->length + 1);
    if (!*key_pem) {
        BIO_free(bio);
        return -1;
    }

    memcpy(*key_pem, mem->data, mem->length);
    (*key_pem)[mem->length] = '\0';
    *pem_len = mem->length + 1;

    BIO_free(bio);
    return 0;
}

int x509_generate_tls_cert_pem_ecdsa(
    const char *common_name,
    uint32_t valid_days,
    char **cert_pem,
    size_t *cert_len,
    char **key_pem,
    size_t *key_len
) {
    if (!common_name || !cert_pem || !cert_len || !key_pem || !key_len) {
        return -1;
    }

    // Generate ECDSA P-256 keypair
    EVP_PKEY *pkey = NULL;
    uint8_t pub_bytes[65];
    size_t pub_len = sizeof(pub_bytes);

    if (x509_generate_ecdsa_keypair_internal(&pkey, pub_bytes, &pub_len) != 0) {
        return -1;
    }

    // Generate self-signed certificate
    uint8_t *cert_der = NULL;
    size_t cert_der_len = 0;

    if (x509_generate_self_signed_ecdsa(common_name, valid_days, pub_bytes, pub_len, pkey, &cert_der, &cert_der_len) != 0) {
        EVP_PKEY_free(pkey);
        return -1;
    }

    // Convert certificate to PEM
    if (x509_cert_to_pem(cert_der, cert_der_len, cert_pem, cert_len) != 0) {
        free(cert_der);
        EVP_PKEY_free(pkey);
        return -1;
    }

    // Convert private key to PEM
    if (x509_privkey_ecdsa_to_pem(pkey, key_pem, key_len) != 0) {
        free(cert_der);
        free(*cert_pem);
        EVP_PKEY_free(pkey);
        return -1;
    }

    // Cleanup
    free(cert_der);
    EVP_PKEY_free(pkey);

    return 0;
}
