/**
 * @file snmp_usm.c
 * @brief SNMPv3 USM cryptographic primitives
 */

#include "snmp_usm.h"
#include "snmp_usm_wire.h"
#include "asn1_types.h"
#include "memory_pool.h"
#include <salts/clock.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>
/* OpenSSL headers */
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/des.h>
#include <openssl/aes.h>

/*
 * Password-to-key derivation (RFC 3414 Section 2.6)
 *
 * Algorithm:
 * 1. Repeat password to fill 1MB buffer
 * 2. Hash the 1MB buffer
 * 3. Result is the key
 */
int usm_password_to_key(
    const char *password,
    snmp_auth_protocol_t auth_protocol,
    uint8_t *key,
    size_t *key_len
) {
    if (!password || !key || !key_len) {
        return USM_ERROR_INVALID;
    }

    size_t password_len = strlen(password);
    if (password_len == 0) {
        return USM_ERROR_INVALID;
    }

    /* Allocate 1MB buffer for password expansion */
    uint8_t *buffer = (uint8_t *)malloc(1048576);
    if (!buffer) {
        return USM_ERROR_INVALID;
    }

    /* Fill buffer with repeated password (RFC 3414 Section 2.6) */
    for (size_t i = 0; i < 1048576; i++) {
        buffer[i] = (uint8_t)password[i % password_len];
    }

    /* Hash the 1MB buffer */
    int result = USM_OK;
    switch (auth_protocol) {
        case SNMP_AUTH_MD5:
            MD5(buffer, 1048576, key);
            *key_len = 16;
            break;
        case SNMP_AUTH_SHA1:
            SHA1(buffer, 1048576, key);
            *key_len = 20;
            break;
        case SNMP_AUTH_SHA256:
            SHA256(buffer, 1048576, key);
            *key_len = 32;
            break;
        default:
            result = USM_ERROR_UNSUPPORTED;
            break;
    }

    free(buffer);
    return result;
}

/*
 * Key localization (RFC 3414 Section 2.6)
 *
 * Algorithm:
 * localized_key = Hash(key || engineID || key)
 */
int usm_localize_key(
    const uint8_t *key,
    size_t key_len,
    const uint8_t *engine_id,
    size_t engine_id_len,
    snmp_auth_protocol_t auth_protocol,
    uint8_t *localized_key,
    size_t *localized_key_len
) {
    if (!key || !engine_id || !localized_key || !localized_key_len) {
        return USM_ERROR_INVALID;
    }

    /* Build concatenation: key || engineID || key */
    uint8_t buffer[1024];
    size_t total_len = key_len + engine_id_len + key_len;

    if (total_len > sizeof(buffer)) {
        return USM_ERROR_INVALID;
    }

    size_t offset = 0;
    memcpy(buffer + offset, key, key_len);
    offset += key_len;
    memcpy(buffer + offset, engine_id, engine_id_len);
    offset += engine_id_len;
    memcpy(buffer + offset, key, key_len);
    offset += key_len;

    /* Hash the concatenation */
    int result = USM_OK;
    switch (auth_protocol) {
        case SNMP_AUTH_MD5:
            MD5(buffer, offset, localized_key);
            *localized_key_len = 16;
            break;
        case SNMP_AUTH_SHA1:
            SHA1(buffer, offset, localized_key);
            *localized_key_len = 20;
            break;
        case SNMP_AUTH_SHA256:
            SHA256(buffer, offset, localized_key);
            *localized_key_len = 32;
            break;
        default:
            result = USM_ERROR_UNSUPPORTED;
            break;
    }

    return result;
}

/*
 * Compute HMAC authentication code
 */
int usm_compute_auth(
    const uint8_t *message,
    size_t message_len,
    const uint8_t *key,
    size_t key_len,
    snmp_auth_protocol_t auth_protocol,
    uint8_t *auth_params
) {
    if (!message || !key || !auth_params) {
        return USM_ERROR_INVALID;
    }

    uint8_t hmac[EVP_MAX_MD_SIZE];
    unsigned int hmac_len = 0;

    const EVP_MD *md = NULL;
    switch (auth_protocol) {
        case SNMP_AUTH_MD5:
            md = EVP_md5();
            break;
        case SNMP_AUTH_SHA1:
            md = EVP_sha1();
            break;
        case SNMP_AUTH_SHA256:
            return USM_ERROR_UNSUPPORTED;
        default:
            return USM_ERROR_UNSUPPORTED;
    }

    /* Compute HMAC */
    if (!HMAC(md, key, (int)key_len, message, message_len, hmac, &hmac_len)) {
        return USM_ERROR_INVALID;
    }

    /* Truncate to first 12 bytes (RFC 3414) */
    memcpy(auth_params, hmac, 12);

    return USM_OK;
}

/*
 * Verify HMAC authentication code
 */
int usm_verify_auth(
    const uint8_t *message,
    size_t message_len,
    const uint8_t *key,
    size_t key_len,
    snmp_auth_protocol_t auth_protocol,
    const uint8_t *auth_params
) {
    uint8_t computed[12];
    if (!message || !key || !auth_params) return USM_ERROR_INVALID;
    int result = usm_compute_auth(message, message_len, key, key_len, auth_protocol, computed);

    if (result != USM_OK) {
        return result;
    }

    /* Constant-time comparison */
    int diff = 0;
    for (int i = 0; i < 12; i++) {
        diff |= (computed[i] ^ auth_params[i]);
    }

    return (diff == 0) ? USM_OK : USM_ERROR_AUTH_FAILED;
}

/*
 * Encrypt data (DES-CBC or AES-CFB)
 */
int usm_encrypt(
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
) {
    if (!plaintext || !key || !salt_out || !ciphertext || !ciphertext_len) {
        return USM_ERROR_INVALID;
    }

    /* Generate salt (8 bytes) */
    static uint32_t counter = 0;
    if (counter == 0) {
        counter = (uint32_t)time(NULL);
    }
    counter++;

    memcpy(salt_out, &engine_boots, 4);
    memcpy(salt_out + 4, &counter, 4);

    int result = USM_OK;

    if (priv_protocol == SNMP_PRIV_DES) {
        /* DES-CBC encryption (RFC 3414) */
        if (key_len < 16 || plaintext_len > (size_t)LONG_MAX ||
            plaintext_len > SIZE_MAX - 7u) {
            return USM_ERROR_INVALID;
        }

        size_t padded_len = ((plaintext_len + 7u) / 8u) * 8u;
        if (*ciphertext_len < padded_len) return USM_ERROR_INVALID;

        /* Pre-IV: salt XOR key[8..15] */
        uint8_t iv[8];
        for (int i = 0; i < 8; i++) {
            iv[i] = salt_out[i] ^ key[8 + i];
        }

        /* DES key is key[0..7] */
        DES_cblock des_key;
        memcpy(&des_key, key, 8);

        DES_key_schedule schedule;
        DES_set_key_unchecked(&des_key, &schedule);

        /* Pad plaintext to 8-byte blocks */
        uint8_t *padded = (uint8_t *)malloc(padded_len);
        if (!padded) {
            return USM_ERROR_INVALID;
        }

        memcpy(padded, plaintext, plaintext_len);
        /* PKCS#5 padding */
        uint8_t pad_byte = (uint8_t)(padded_len - plaintext_len);
        for (size_t i = plaintext_len; i < padded_len; i++) {
            padded[i] = pad_byte;
        }

        DES_ncbc_encrypt(padded, ciphertext, (long)padded_len, &schedule,
                         (DES_cblock *)iv, DES_ENCRYPT);
        *ciphertext_len = padded_len;

        free(padded);

    } else if (priv_protocol == SNMP_PRIV_AES128 || priv_protocol == SNMP_PRIV_AES256) {
        /* AES-CFB encryption (RFC 3826) */
        int key_bits = (priv_protocol == SNMP_PRIV_AES128) ? 128 : 256;
        size_t required_key_len = key_bits / 8;

        if (key_len < required_key_len || *ciphertext_len < plaintext_len) {
            return USM_ERROR_INVALID;
        }

        /* IV = engineBoots || engineTime || salt */
        uint8_t iv[16];
        memcpy(iv, &engine_boots, 4);
        memcpy(iv + 4, &engine_time, 4);
        memcpy(iv + 8, salt_out, 8);

        AES_KEY aes_key;
        if (AES_set_encrypt_key(key, key_bits, &aes_key) != 0) {
            return USM_ERROR_INVALID;
        }

        int num = 0;
        AES_cfb128_encrypt(plaintext, ciphertext, plaintext_len,
                           &aes_key, iv, &num, AES_ENCRYPT);
        *ciphertext_len = plaintext_len;  /* CFB mode, no padding */

    } else {
        result = USM_ERROR_UNSUPPORTED;
    }

    return result;
}

/*
 * Decrypt data
 */
int usm_decrypt(
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
) {
    if (!ciphertext || !key || !salt || !plaintext || !plaintext_len) {
        return USM_ERROR_INVALID;
    }

    int result = USM_OK;

    if (priv_protocol == SNMP_PRIV_DES) {
        /* DES-CBC decryption (RFC 3414) */
        if (key_len < 16 || ciphertext_len % 8 != 0 ||
            ciphertext_len > (size_t)LONG_MAX || *plaintext_len < ciphertext_len) {
            return USM_ERROR_INVALID;
        }

        /* Pre-IV: salt XOR key[8..15] */
        uint8_t iv[8];
        for (int i = 0; i < 8; i++) {
            iv[i] = salt[i] ^ key[8 + i];
        }

        /* DES key is key[0..7] */
        DES_cblock des_key;
        memcpy(&des_key, key, 8);

        DES_key_schedule schedule;
        DES_set_key_unchecked(&des_key, &schedule);

        DES_ncbc_encrypt(ciphertext, plaintext, (long)ciphertext_len, &schedule,
                         (DES_cblock *)iv, DES_DECRYPT);

        /* Remove PKCS#5 padding */
        if (ciphertext_len > 0) {
            uint8_t pad_byte = plaintext[ciphertext_len - 1];
            if (pad_byte > 0 && pad_byte <= 8) {
                /* Verify padding */
                int valid = 1;
                for (size_t i = ciphertext_len - pad_byte; i < ciphertext_len; i++) {
                    if (plaintext[i] != pad_byte) {
                        valid = 0;
                        break;
                    }
                }
                if (valid) {
                    *plaintext_len = ciphertext_len - pad_byte;
                } else {
                    *plaintext_len = ciphertext_len;
                    result = USM_ERROR_DECRYPT;
                }
            } else {
                *plaintext_len = ciphertext_len;
            }
        } else {
            *plaintext_len = 0;
        }

    } else if (priv_protocol == SNMP_PRIV_AES128 || priv_protocol == SNMP_PRIV_AES256) {
        /* AES-CFB decryption (RFC 3826) */
        int key_bits = (priv_protocol == SNMP_PRIV_AES128) ? 128 : 256;
        size_t required_key_len = key_bits / 8;

        if (key_len < required_key_len || *plaintext_len < ciphertext_len) {
            return USM_ERROR_INVALID;
        }

        /* IV = engineBoots || engineTime || salt */
        uint8_t iv[16];
        memcpy(iv, &engine_boots, 4);
        memcpy(iv + 4, &engine_time, 4);
        memcpy(iv + 8, salt, 8);

        AES_KEY aes_key;
        if (AES_set_encrypt_key(key, key_bits, &aes_key) != 0) {
            return USM_ERROR_INVALID;
        }

        int num = 0;
        AES_cfb128_encrypt(ciphertext, plaintext, ciphertext_len,
                           &aes_key, iv, &num, AES_DECRYPT);
        *plaintext_len = ciphertext_len;  /* CFB mode, no padding */

    } else {
        result = USM_ERROR_UNSUPPORTED;
    }

    return result;
}

/*
 * Create user from passwords
 */
int usm_create_user(
    const char *user_name,
    const char *auth_password,
    snmp_auth_protocol_t auth_protocol,
    const char *priv_password,
    snmp_priv_protocol_t priv_protocol,
    const uint8_t *engine_id,
    size_t engine_id_len,
    snmp_v3_user_t *user
) {
    if (!user_name || !user) {
        return USM_ERROR_INVALID;
    }

    memset(user, 0, sizeof(*user));
    user->user_name = strdup(user_name);
    user->auth_protocol = auth_protocol;
    user->priv_protocol = priv_protocol;

    /* Derive and localize authentication key */
    if (auth_password && auth_protocol != SNMP_AUTH_NONE) {
        uint8_t master_key[32];
        size_t master_key_len;

        int result = usm_password_to_key(auth_password, auth_protocol, master_key, &master_key_len);
        if (result != USM_OK) {
            free(user->user_name);
            return result;
        }

        result = usm_localize_key(
            master_key, master_key_len,
            engine_id, engine_id_len,
            auth_protocol,
            user->auth_key, &user->auth_key_len
        );

        if (result != USM_OK) {
            free(user->user_name);
            return result;
        }
    }

    /* Derive and localize privacy key */
    if (priv_password && priv_protocol != SNMP_PRIV_NONE) {
        uint8_t master_key[32];
        size_t master_key_len;

        int result = usm_password_to_key(priv_password, auth_protocol, master_key, &master_key_len);
        if (result != USM_OK) {
            free(user->user_name);
            return result;
        }

        result = usm_localize_key(
            master_key, master_key_len,
            engine_id, engine_id_len,
            auth_protocol,
            user->priv_key, &user->priv_key_len
        );

        if (result != USM_OK) {
            free(user->user_name);
            return result;
        }
    }

    return USM_OK;
}

/*
 * Encode USM security parameters to ASN.1/BER
 *
 * RFC 3414 Section 2.4:
 * UsmSecurityParameters ::= SEQUENCE {
 *     msgAuthoritativeEngineID     OCTET STRING,
 *     msgAuthoritativeEngineBoots  INTEGER (0..2147483647),
 *     msgAuthoritativeEngineTime   INTEGER (0..2147483647),
 *     msgUserName                  OCTET STRING (SIZE(0..32)),
 *     msgAuthenticationParameters  OCTET STRING,
 *     msgPrivacyParameters         OCTET STRING
 * }
 */
int snmp_usm_encode_security_params_sized(
    const snmp_usm_params_t *params,
    size_t auth_params_len,
    size_t priv_params_len,
    uint8_t *out,
    size_t *out_len
) {
    if (!params || !out || !out_len || auth_params_len > sizeof(params->auth_params) ||
        priv_params_len > sizeof(params->priv_params)) {
        return USM_ERROR_INVALID;
    }

    /* Build SEQUENCE */
    asn1_value_t *seq = asn1_create_sequence();
    if (!seq) {
        return USM_ERROR_INVALID;
    }

    /* 1. msgAuthoritativeEngineID - OCTET STRING */
    asn1_value_t *engine_id = asn1_create_octet_string(
        params->authoritative_engine_id,
        params->engine_id_len
    );
    if (!engine_id) {
        asn1_free(seq);
        return USM_ERROR_INVALID;
    }
    asn1_sequence_add_child(seq, engine_id);

    /* 2. msgAuthoritativeEngineBoots - INTEGER */
    asn1_value_t *boots = asn1_create_integer((int64_t)params->engine_boots);
    if (!boots) {
        asn1_free(seq);
        return USM_ERROR_INVALID;
    }
    asn1_sequence_add_child(seq, boots);

    /* 3. msgAuthoritativeEngineTime - INTEGER */
    asn1_value_t *time = asn1_create_integer((int64_t)params->engine_time);
    if (!time) {
        asn1_free(seq);
        return USM_ERROR_INVALID;
    }
    asn1_sequence_add_child(seq, time);

    /* 4. msgUserName - OCTET STRING */
    size_t user_name_len = params->user_name ? strlen(params->user_name) : 0;
    asn1_value_t *user_name = asn1_create_octet_string(
        (const uint8_t *)params->user_name,
        user_name_len
    );
    if (!user_name) {
        asn1_free(seq);
        return USM_ERROR_INVALID;
    }
    asn1_sequence_add_child(seq, user_name);

    /* 5. msgAuthenticationParameters - OCTET STRING (12 bytes) */
    asn1_value_t *auth_params = asn1_create_octet_string(
        params->auth_params,
        auth_params_len
    );
    if (!auth_params) {
        asn1_free(seq);
        return USM_ERROR_INVALID;
    }
    asn1_sequence_add_child(seq, auth_params);

    /* 6. msgPrivacyParameters - OCTET STRING (8 bytes) */
    asn1_value_t *priv_params = asn1_create_octet_string(
        params->priv_params,
        priv_params_len
    );
    if (!priv_params) {
        asn1_free(seq);
        return USM_ERROR_INVALID;
    }
    asn1_sequence_add_child(seq, priv_params);

    /* Encode to BER */
    int result = asn1_der_encode(seq, out, out_len);
    asn1_free(seq);

    return (result == 0) ? USM_OK : USM_ERROR_INVALID;
}

int usm_encode_security_params(
    const snmp_usm_params_t *params,
    uint8_t *out,
    size_t *out_len
) {
    return snmp_usm_encode_security_params_sized(
        params, sizeof(params->auth_params), sizeof(params->priv_params), out, out_len);
}

/*
 * Decode USM security parameters from ASN.1/BER
 */
int usm_decode_security_params(
    const uint8_t *data,
    size_t len,
    snmp_usm_params_t *params,
    void *pool
) {
    uint8_t *engine_id_copy = NULL;
    if (!data || !params) {
        return USM_ERROR_INVALID;
    }

    MemoryPool *mem_pool = (MemoryPool *)pool;

    /* Decode SEQUENCE */
    asn1_value_t *root = NULL;
    int result = scan_binary_asn1(data, len, &root);
    if (result != 0 || !root || root->tag != 0x30) {
        if (root) {
            asn1_free(root);
        }
        return USM_ERROR_INVALID;
    }

    /* Must have 6 children */
    if (root->value.sequence.count != 6) {
        asn1_free(root);
        return USM_ERROR_INVALID;
    }

    asn1_value_t **children = root->value.sequence.children;

    /* 1. msgAuthoritativeEngineID - OCTET STRING */
    if (children[0]->tag != 0x04) {
        asn1_free(root);
        return USM_ERROR_INVALID;
    }
    params->engine_id_len = children[0]->value.octet_string.length;
    if (params->engine_id_len > 0) {
        engine_id_copy = mem_pool ? (uint8_t *)pool_alloc(mem_pool, params->engine_id_len)
                                  : (uint8_t *)malloc(params->engine_id_len);
        if (!engine_id_copy) {
            asn1_free(root);
            return USM_ERROR_INVALID;
        }
        memcpy(engine_id_copy, children[0]->value.octet_string.data, params->engine_id_len);
    }
    params->authoritative_engine_id = engine_id_copy;

    /* 2. msgAuthoritativeEngineBoots - INTEGER */
    if (children[1]->tag != 0x02) {
        asn1_free(root);
        return USM_ERROR_INVALID;
    }
    params->engine_boots = (uint32_t)children[1]->value.integer;

    /* 3. msgAuthoritativeEngineTime - INTEGER */
    if (children[2]->tag != 0x02) {
        asn1_free(root);
        return USM_ERROR_INVALID;
    }
    params->engine_time = (uint32_t)children[2]->value.integer;

    /* 4. msgUserName - OCTET STRING */
    if (children[3]->tag != 0x04) {
        asn1_free(root);
        return USM_ERROR_INVALID;
    }
    /* Need null-terminated string */
    size_t user_name_len = children[3]->value.octet_string.length;
    char *user_name_str =
        mem_pool ? (char *)pool_alloc(mem_pool, user_name_len + 1) : (char *)malloc(user_name_len + 1);
    if (!user_name_str) {
        asn1_free(root);
        return USM_ERROR_INVALID;
    }
    memcpy(user_name_str, children[3]->value.octet_string.data, user_name_len);
    user_name_str[user_name_len] = '\0';
    params->user_name = user_name_str;

    /* 5. msgAuthenticationParameters - OCTET STRING */
    if (children[4]->tag != 0x04 || children[4]->value.octet_string.length > 12) {
        asn1_free(root);
        return USM_ERROR_INVALID;
    }
    memset(params->auth_params, 0, sizeof(params->auth_params));
    if (children[4]->value.octet_string.length > 0u) {
        memcpy(params->auth_params, children[4]->value.octet_string.data,
               children[4]->value.octet_string.length);
    }

    /* 6. msgPrivacyParameters - OCTET STRING */
    if (children[5]->tag != 0x04 || children[5]->value.octet_string.length > 8) {
        asn1_free(root);
        return USM_ERROR_INVALID;
    }
    memset(params->priv_params, 0, sizeof(params->priv_params));
    if (children[5]->value.octet_string.length > 0u) {
        memcpy(params->priv_params, children[5]->value.octet_string.data,
               children[5]->value.octet_string.length);
    }

    asn1_free(root);
    return USM_OK;
}

/*
 * Get current timestamp in milliseconds
 */
static uint64_t get_current_time_ms(void) {
    return salts_monotonic_ms();
}

/*
 * Initialize engine time state
 */
void usm_engine_time_init(snmp_engine_time_t *state, uint32_t boots) {
    if (!state) return;

    state->engine_boots = boots;
    state->engine_time = 0;
    state->last_update_time = get_current_time_ms();
}

/*
 * Get current engine boots and time
 */
void usm_engine_time_get(
    snmp_engine_time_t *state,
    uint32_t *boots,
    uint32_t *time
) {
    if (!state || !boots || !time) return;

    /* Calculate elapsed seconds since last update */
    uint64_t now = get_current_time_ms();
    uint64_t elapsed_ms = now - state->last_update_time;
    uint32_t elapsed_seconds = (uint32_t)(elapsed_ms / 1000);

    /* Update engine time */
    state->engine_time += elapsed_seconds;
    state->last_update_time = now;

    /* Check for overflow (max 2^31-1 seconds ~= 68 years) */
    if (state->engine_time > 2147483647) {
        state->engine_boots++;
        state->engine_time = 0;
    }

    *boots = state->engine_boots;
    *time = state->engine_time;
}

/*
 * Update cached remote engine time
 */
void usm_engine_time_update(
    snmp_engine_time_t *state,
    uint32_t boots,
    uint32_t time
) {
    if (!state) return;

    state->engine_boots = boots;
    state->engine_time = time;
    state->last_update_time = get_current_time_ms();
}

/*
 * Verify received message time is within acceptable window
 *
 * RFC 3414 Section 3.2:
 * The message is considered to be outside of the Time Window if:
 * - msgAuthoritativeEngineBoots < snmpEngineBoots, OR
 * - msgAuthoritativeEngineBoots == snmpEngineBoots AND
 *   |msgAuthoritativeEngineTime - snmpEngineTime| > 150 seconds
 */
int usm_verify_time_window(
    const snmp_engine_time_t *local_state,
    uint32_t msg_boots,
    uint32_t msg_time
) {
    if (!local_state) {
        return USM_ERROR_INVALID;
    }

    /* Get current local time */
    uint64_t now = get_current_time_ms();
    uint64_t elapsed_ms = now - local_state->last_update_time;
    uint32_t current_time = local_state->engine_time + (uint32_t)(elapsed_ms / 1000);

    /* Check boots */
    if (msg_boots < local_state->engine_boots) {
        return USM_ERROR_AUTH_FAILED;  /* Message from old boot */
    }

    if (msg_boots == local_state->engine_boots) {
        /* Check time window (±150 seconds) */
        int32_t time_diff = (int32_t)msg_time - (int32_t)current_time;
        if (time_diff < -150 || time_diff > 150) {
            return USM_ERROR_AUTH_FAILED;  /* Outside time window */
        }
    }

    /* If msg_boots > local_boots, accept (we're behind) */
    return USM_OK;
}
