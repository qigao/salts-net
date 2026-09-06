/**
 * @file snmp_usm.h
 * @brief SNMPv3 User-based Security Model (RFC 3414)
 *
 * Implements:
 * - Key derivation (password → key)
 * - Key localization (key + engineID → localized key)
 * - Authentication (HMAC-MD5, HMAC-SHA-1, HMAC-SHA-256)
 * - Privacy (DES-CBC, AES-128-CFB, AES-256-CFB)
 */

#ifndef SNMP_USM_H
#define SNMP_USM_H


#include "snmp_api.h"
#include "platform.h"
#include "snmp_types.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define USM_OK                  0
#define USM_ERROR_INVALID      -1
#define USM_ERROR_AUTH_FAILED  -2
#define USM_ERROR_DECRYPT      -3
#define USM_ERROR_UNSUPPORTED  -4

/**
 * Password-to-key derivation (RFC 3414 Section 2.6)
 *
/**
 * Password-to-key derivation (RFC 3414 Section 2.6)
 *
 * @param password Password string
 * @param auth_protocol Authentication protocol
 * @param key Output key buffer (min 32 bytes)
 * @param key_len Output key length
 * @return USM_OK on success, negative error code on failure
 *
 * Example:
 *   uint8_t key[32];
 *   size_t key_len;
 *   usm_password_to_key("mypassword", SNMP_AUTH_SHA1, key, &key_len);
 */
SALTSNET_SNMP_C_API int usm_password_to_key(
    const char *password,
    snmp_auth_protocol_t auth_protocol,
    uint8_t *key,
    size_t *key_len
);

/**
 * Key localization (RFC 3414 Section 2.6)
 *
 * Localizes a key to a specific SNMP engine using engineID.
 *
 * @param key Input master key
 * @param key_len Input key length
 * @param engine_id Engine ID
 * @param engine_id_len Engine ID length
 * @param auth_protocol Authentication protocol
 * @param localized_key Output localized key (min 32 bytes)
 * @param localized_key_len Output key length
 * @return USM_OK on success, negative error code on failure
 */
SALTSNET_SNMP_C_API int usm_localize_key(
    const uint8_t *key,
    size_t key_len,
    const uint8_t *engine_id,
    size_t engine_id_len,
    snmp_auth_protocol_t auth_protocol,
    uint8_t *localized_key,
    size_t *localized_key_len
);

/**
 * Compute authentication code (HMAC)
 *
 * @param message Message to authenticate
 * @param message_len Message length
 * @param key Authentication key
 * @param key_len Key length
 * @param auth_protocol Authentication protocol
 * @param auth_params Output authentication parameters (12 bytes)
 * @return USM_OK on success, negative error code on failure
 */
SALTSNET_SNMP_C_API int usm_compute_auth(
    const uint8_t *message,
    size_t message_len,
    const uint8_t *key,
    size_t key_len,
    snmp_auth_protocol_t auth_protocol,
    uint8_t *auth_params
);

/**
 * Verify authentication code
 *
 * @param message Message to verify
 * @param message_len Message length
 * @param key Authentication key
 * @param key_len Key length
 * @param auth_protocol Authentication protocol
 * @param auth_params Received authentication parameters (12 bytes)
 * @return USM_OK if valid, USM_ERROR_AUTH_FAILED if invalid
 */
SALTSNET_SNMP_C_API int usm_verify_auth(
    const uint8_t *message,
    size_t message_len,
    const uint8_t *key,
    size_t key_len,
    snmp_auth_protocol_t auth_protocol,
    const uint8_t *auth_params
);

/**
 * Encrypt data (privacy)
 *
 * @param plaintext Plaintext data
 * @param plaintext_len Plaintext length
 * @param key Privacy key
 * @param key_len Key length
 * @param priv_protocol Privacy protocol
 * @param engine_boots Engine boots
 * @param engine_time Engine time
 * @param salt_out Output salt/IV (8 bytes)
 * @param ciphertext Output ciphertext buffer
 * @param ciphertext_len [in] Buffer size, [out] Ciphertext length
 * @return USM_OK on success, negative error code on failure
 */
SALTSNET_SNMP_C_API int usm_encrypt(
    const uint8_t *plaintext,
    size_t plaintext_len,
    const uint8_t *key,
    size_t key_len,
    snmp_priv_protocol_t priv_protocol,
    uint32_t engine_boots,
    uint32_t engine_time,
    uint8_t *salt_out,
    uint8_t *ciphertext,
    size_t *ciphertext_len
);

/**
 * Decrypt data (privacy)
 *
 * @param ciphertext Ciphertext data
 * @param ciphertext_len Ciphertext length
 * @param key Privacy key
 * @param key_len Key length
 * @param priv_protocol Privacy protocol
 * @param engine_boots Engine boots
 * @param engine_time Engine time
 * @param salt Salt/IV (8 bytes)
 * @param plaintext Output plaintext buffer
 * @param plaintext_len [in] Buffer size, [out] Plaintext length
 * @return USM_OK on success, negative error code on failure
 */
SALTSNET_SNMP_C_API int usm_decrypt(
    const uint8_t *ciphertext,
    size_t ciphertext_len,
    const uint8_t *key,
    size_t key_len,
    snmp_priv_protocol_t priv_protocol,
    uint32_t engine_boots,
    uint32_t engine_time,
    const uint8_t *salt,
    uint8_t *plaintext,
    size_t *plaintext_len
);

/**
 * Create SNMPv3 user from password
 *
 * @param user_name User name
 * @param auth_password Authentication password (can be NULL if no auth)
 * @param auth_protocol Authentication protocol
 * @param priv_password Privacy password (can be NULL if no privacy)
 * @param priv_protocol Privacy protocol
 * @param engine_id Engine ID for key localization
 * @param engine_id_len Engine ID length
 * @param user Output user structure
 * @return USM_OK on success, negative error code on failure
 */
SALTSNET_SNMP_C_API int usm_create_user(
    const char *user_name,
    const char *auth_password,
    snmp_auth_protocol_t auth_protocol,
    const char *priv_password,
    snmp_priv_protocol_t priv_protocol,
    const uint8_t *engine_id,
    size_t engine_id_len,
    snmp_v3_user_t *user
);

/**
 * Engine time state
 *
 * Tracks local engine boots/time or remote engine time cache.
 */
typedef struct {
    uint32_t engine_boots;         /* Engine boots count */
    uint32_t engine_time;          /* Engine time (seconds) */
    uint64_t last_update_time;     /* Last update timestamp (ms) */
} snmp_engine_time_t;

/**
 * Initialize engine time state
 *
 * @param state Engine time state to initialize
 * @param boots Initial boots count (0 for new engine, loaded from persistence)
 */
SALTSNET_SNMP_C_API void usm_engine_time_init(snmp_engine_time_t *state, uint32_t boots);

/**
 * Get current engine boots and time
 *
 * @param state Engine time state
 * @param boots Output engine boots
 * @param time Output engine time
 */
SALTSNET_SNMP_C_API void usm_engine_time_get(
    snmp_engine_time_t *state,
    uint32_t *boots,
    uint32_t *time
);

/**
 * Update cached remote engine time
 *
 * Call this when receiving an authoritative message from a remote engine.
 *
 * @param state Engine time state
 * @param boots Remote engine boots
 * @param time Remote engine time
 */
SALTSNET_SNMP_C_API void usm_engine_time_update(
    snmp_engine_time_t *state,
    uint32_t boots,
    uint32_t time
);

/**
 * Verify received message time is within acceptable window
 *
 * RFC 3414 requires messages to be within ±150 seconds of local time.
 *
 * @param local_state Local engine time state
 * @param msg_boots Received message boots
 * @param msg_time Received message time
 * @return USM_OK if time is valid, USM_ERROR_AUTH_FAILED if not
 */
SALTSNET_SNMP_C_API int usm_verify_time_window(
    const snmp_engine_time_t *local_state,
    uint32_t msg_boots,
    uint32_t msg_time
);

/**
 * Encode USM security parameters to ASN.1/BER
 *
 * Encodes UsmSecurityParameters SEQUENCE as defined in RFC 3414.
 *
 * @param params USM parameters to encode
 * @param out Output buffer
 * @param out_len [in] Buffer size, [out] Encoded length
 * @return USM_OK on success, negative error code on failure
 */
SALTSNET_SNMP_C_API int usm_encode_security_params(
    const snmp_usm_params_t *params,
    uint8_t *out,
    size_t *out_len
);

/**
 * Decode USM security parameters from ASN.1/BER
 *
 * Decodes UsmSecurityParameters SEQUENCE as defined in RFC 3414.
 *
 * @param data Input BER data
 * @param len Input data length
 * @param params Output USM parameters
 * @param pool Memory pool for allocations (optional)
 * @return USM_OK on success, negative error code on failure
 */
SALTSNET_SNMP_C_API int usm_decode_security_params(
    const uint8_t *data,
    size_t len,
    snmp_usm_params_t *params,
    void *pool
);

#ifdef __cplusplus
}
#endif

#endif /* SNMP_USM_H */
