/**
 * @file test_snmp_v3.c
 * @brief SNMPv3 USM Security Tests
 */

#include "snmp_usm.h"
#include "snmp_builder.h"
#include "snmp_parser.h"
#include "asn1_types.h"
#include "tinytest.h"
#include <stdlib.h>
#include <string.h>

static size_t find_test_bytes(const uint8_t *data, size_t data_len,
                              const uint8_t *needle, size_t needle_len) {
    if (!data || !needle || needle_len == 0 || needle_len > data_len) return SIZE_MAX;
    for (size_t offset = 0; offset <= data_len - needle_len; ++offset) {
        if (memcmp(data + offset, needle, needle_len) == 0) return offset;
    }
    return SIZE_MAX;
}

static int build_test_auth_request(uint8_t *request, size_t *request_len,
                                   snmp_v3_user_t *user, snmp_oid_t *oid) {
    static uint8_t engine_id[] = {0x80, 0x00, 0x1f, 0x88, 0x80, 0x12, 0x34};
    snmp_usm_params_t usm_params = {0};
    int result = usm_create_user("authuser", "myauthpassword", SNMP_AUTH_SHA1,
                                 NULL, SNMP_PRIV_NONE, engine_id,
                                 sizeof(engine_id), user);
    if (result != USM_OK) return result;
    result = snmp_oid_from_string("1.3.6.1.2.1.1.3.0", oid);
    if (result != SNMP_BUILD_OK) {
        free(user->user_name);
        user->user_name = NULL;
        return result;
    }
    usm_params.authoritative_engine_id = engine_id;
    usm_params.engine_id_len = sizeof(engine_id);
    usm_params.engine_boots = 10;
    usm_params.engine_time = 5000;
    usm_params.user_name = "authuser";
    result = snmp_build_v3_get_request(99999, oid, 1, &usm_params, user,
                                       SNMP_SEC_LEVEL_AUTH_NOPRIV,
                                       request, request_len);
    if (result != SNMP_BUILD_OK) {
        snmp_oid_free(oid);
        free(user->user_name);
        user->user_name = NULL;
    }
    return result;
}

static int test_usm_parameter_lengths(const uint8_t *message, size_t message_len,
                                      size_t *auth_len, size_t *priv_len) {
    asn1_value_t *root = NULL;
    asn1_value_t *usm = NULL;
    int result = -1;
    if (!message || !auth_len || !priv_len ||
        scan_binary_asn1(message, message_len, &root) != 0 || !root ||
        root->tag != 0x30 || root->value.sequence.count != 4) {
        goto cleanup;
    }
    asn1_value_t *security = root->value.sequence.children[2];
    if (security->tag != 0x04 ||
        scan_binary_asn1(security->value.octet_string.data,
                         security->value.octet_string.length, &usm) != 0 ||
        !usm || usm->tag != 0x30 || usm->value.sequence.count != 6) {
        goto cleanup;
    }
    if (usm->value.sequence.children[4]->tag != 0x04 ||
        usm->value.sequence.children[5]->tag != 0x04) {
        goto cleanup;
    }
    *auth_len = usm->value.sequence.children[4]->value.octet_string.length;
    *priv_len = usm->value.sequence.children[5]->value.octet_string.length;
    result = 0;

cleanup:
    if (usm) asn1_free(usm);
    if (root) asn1_free(root);
    return result;
}

spec("snmp_v3") {
  describe("CMeta protocol metadata") {
    it("reflects stable SNMPv3 security values and names") {
        snmp_auth_protocol_t parsed_auth = SNMP_AUTH_NONE;
        snmp_priv_protocol_t parsed_priv = SNMP_PRIV_NONE;

        check_equal((int)SNMP_VERSION_3, 3);
        check_equal((int)SNMP_SEC_LEVEL_AUTH_PRIV, 3);
        check_equal((int)SNMP_AUTH_SHA256, 3);
        check_equal((int)SNMP_PRIV_AES256, 3);
        check_equal(snmp_version_t_meta()->count, (size_t)3);
        check_equal(snmp_security_level_t_to_string(SNMP_SEC_LEVEL_AUTH_NOPRIV),
                    "auth_no_priv");
        check_true(snmp_auth_protocol_t_from_string("sha1", &parsed_auth));
        check_equal(parsed_auth, SNMP_AUTH_SHA1);
        check_true(snmp_priv_protocol_t_from_string("aes256", &parsed_priv));
        check_equal(parsed_priv, SNMP_PRIV_AES256);
    }
  }

  describe("USM Password-to-Key Derivation (RFC 3414)") {
    it("should derive MD5 key from password correctly") {
        const char *password = "maplesyrup";
        uint8_t key[32];
        size_t key_len;

        int result = usm_password_to_key(password, SNMP_AUTH_MD5, key, &key_len);

        check_equal(result, USM_OK);
        check_equal(key_len, 16);
    }

    it("should derive SHA1 key from password correctly") {
        const char *password = "maplesyrup";
        uint8_t key[32];
        size_t key_len;

        int result = usm_password_to_key(password, SNMP_AUTH_SHA1, key, &key_len);

        check_equal(result, USM_OK);
        check_equal(key_len, 20);
    }
  }

  describe("USM Key Localization") {
    it("should successfully localize a master key for a specific engine ID") {
        /* First derive a key from password */
        const char *password = "testpassword";
        uint8_t master_key[32];
        size_t master_key_len;

        usm_password_to_key(password, SNMP_AUTH_MD5, master_key, &master_key_len);

        /* Engine ID */
        uint8_t engine_id[] = {
            0x80, 0x00, 0x1f, 0x88, 0x80, 0xe9, 0x63, 0x00,
            0x00, 0xd6, 0x1f, 0xf4, 0x49
        };

        uint8_t localized_key[32];
        size_t localized_key_len;

        int result = usm_localize_key(
            master_key, master_key_len,
            engine_id, sizeof(engine_id),
            SNMP_AUTH_MD5,
            localized_key, &localized_key_len
        );

        check_equal(result, USM_OK);
        check_equal(localized_key_len, 16);

        /* Verify localized key is different from master key */
        check(memcmp(master_key, localized_key, 16) != 0);
    }
  }

  describe("USM Authentication (HMAC)") {
    it("should compute and verify HMAC-MD5 correctly") {
        uint8_t key[16] = {
            0x52, 0x6f, 0x5e, 0xed, 0x9f, 0xcc, 0xe2, 0x6f,
            0x89, 0x64, 0xc2, 0x93, 0x07, 0x87, 0xd8, 0x2b
        };

        uint8_t message[] = "Hello, SNMPv3!";
        uint8_t auth_params[12];

        /* Compute HMAC */
        int result = usm_compute_auth(
            message, sizeof(message) - 1,
            key, 16,
            SNMP_AUTH_MD5,
            auth_params
        );

        check_equal(result, USM_OK);

        /* Verify HMAC */
        result = usm_verify_auth(
            message, sizeof(message) - 1,
            key, 16,
            SNMP_AUTH_MD5,
            auth_params
        );

        check_equal(result, USM_OK);

        /* Verify with corrupted message fails */
        message[0] = 'X';
        result = usm_verify_auth(
            message, sizeof(message) - 1,
            key, 16,
            SNMP_AUTH_MD5,
            auth_params
        );

        check_equal(result, USM_ERROR_AUTH_FAILED);
    }
  }

  describe("USM Privacy (Encryption)") {
    it("should successfully encrypt and decrypt using DES-CBC") {
        uint8_t key[16] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
        };

        uint8_t plaintext[] = "This is a test message for DES encryption.";
        uint8_t ciphertext[128];
        size_t ciphertext_len = sizeof(ciphertext);
        uint8_t salt[8];

        /* Encrypt */
        int result = usm_encrypt(
            plaintext, sizeof(plaintext) - 1,
            key, 16,
            SNMP_PRIV_DES,
            100,  /* engine_boots */
            12345,  /* engine_time */
            salt,
            ciphertext,
            &ciphertext_len
        );

        check_equal(result, USM_OK);
        check(ciphertext_len > 0);

        /* Decrypt */
        uint8_t decrypted[128];
        size_t decrypted_len = sizeof(decrypted);

        result = usm_decrypt(
            ciphertext, ciphertext_len,
            key, 16,
            SNMP_PRIV_DES,
            100,  /* engine_boots */
            12345,  /* engine_time */
            salt,
            decrypted,
            &decrypted_len
        );

        check_equal(result, USM_OK);
        check_equal(decrypted_len, sizeof(plaintext) - 1);
        check_equal(decrypted, plaintext, decrypted_len);
    }

    it("should successfully encrypt and decrypt using AES-128-CFB") {
        uint8_t key[16] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
        };

        uint8_t plaintext[] = "AES-128 CFB mode test";
        uint8_t ciphertext[128];
        size_t ciphertext_len = sizeof(ciphertext);
        uint8_t salt[8];

        /* Encrypt */
        int result = usm_encrypt(
            plaintext, sizeof(plaintext) - 1,
            key, 16,
            SNMP_PRIV_AES128,
            200,  /* engine_boots */
            54321,  /* engine_time */
            salt,
            ciphertext,
            &ciphertext_len
        );

        check_equal(result, USM_OK);
        check_equal(ciphertext_len, sizeof(plaintext) - 1);  /* CFB mode, no padding */

        /* Decrypt */
        uint8_t decrypted[128];
        size_t decrypted_len = sizeof(decrypted);

        result = usm_decrypt(
            ciphertext, ciphertext_len,
            key, 16,
            SNMP_PRIV_AES128,
            200,  /* engine_boots */
            54321,  /* engine_time */
            salt,
            decrypted,
            &decrypted_len
        );

        check_equal(result, USM_OK);
        check_equal(decrypted_len, sizeof(plaintext) - 1);
        check_equal(decrypted, plaintext, decrypted_len);
    }

    it("decrypts the NIST AES-128-CFB vector with the RFC 3826 IV layout") {
        static const uint8_t key[16] = {
            0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
            0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
        static const uint8_t salt[8] = {
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
        static const uint8_t ciphertext[16] = {
            0x3b, 0x3f, 0xd9, 0x2e, 0xb7, 0x2d, 0xad, 0x20,
            0x33, 0x34, 0x49, 0xf8, 0xe8, 0x3c, 0xfb, 0x4a};
        static const uint8_t expected[16] = {
            0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
            0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
        uint8_t plaintext[sizeof(ciphertext)] = {0};
        size_t plaintext_len = sizeof(plaintext);

        check_equal(usm_decrypt(ciphertext, sizeof(ciphertext), key, sizeof(key),
                                SNMP_PRIV_AES128, 0x00010203u, 0x04050607u,
                                salt, plaintext, &plaintext_len),
                    USM_OK);
        check_equal(plaintext_len, sizeof(expected));
        check_equal(plaintext, expected, sizeof(expected));
    }

    it("emits consecutive AES privacy salts in network byte order") {
        uint8_t key[16] = {0};
        uint8_t plaintext[1] = {0};
        uint8_t ciphertext[1] = {0};
        uint8_t first_salt[8] = {0};
        uint8_t second_salt[8] = {0};
        size_t ciphertext_len = sizeof(ciphertext);
        uint64_t first = 0u;
        uint64_t second = 0u;

        check_equal(usm_encrypt(plaintext, sizeof(plaintext), key, sizeof(key),
                                SNMP_PRIV_AES128, 1u, 1u, first_salt,
                                ciphertext, &ciphertext_len),
                    USM_OK);
        ciphertext_len = sizeof(ciphertext);
        check_equal(usm_encrypt(plaintext, sizeof(plaintext), key, sizeof(key),
                                SNMP_PRIV_AES128, 1u, 1u, second_salt,
                                ciphertext, &ciphertext_len),
                    USM_OK);
        for (size_t index = 0u; index < sizeof(first_salt); ++index) {
            first = (first << 8u) | first_salt[index];
            second = (second << 8u) | second_salt[index];
        }
        check_equal(second, first + 1u);
    }

    it("rejects AES encryption when the output capacity is too small") {
        uint8_t key[16] = {0};
        uint8_t plaintext[16] = {0};
        uint8_t salt[8] = {0};
        uint8_t guarded_output[17];
        size_t output_len = 8;
        memset(guarded_output, 0xA5, sizeof(guarded_output));

        check_equal(usm_encrypt(plaintext, sizeof(plaintext), key, sizeof(key),
                                SNMP_PRIV_AES128, 1, 1, salt,
                                guarded_output, &output_len),
                    USM_ERROR_INVALID);
        for (size_t index = 8; index < sizeof(guarded_output); ++index) {
            check_equal(guarded_output[index], (uint8_t)0xA5);
        }
    }

    it("rejects AES decryption when the output capacity is too small") {
        uint8_t key[16] = {0};
        uint8_t plaintext[16] = {0};
        uint8_t salt[8] = {0};
        uint8_t ciphertext[16] = {0};
        uint8_t guarded_output[17];
        size_t ciphertext_len = sizeof(ciphertext);
        size_t output_len = 8;
        memset(guarded_output, 0xA5, sizeof(guarded_output));
        check_equal(usm_encrypt(plaintext, sizeof(plaintext), key, sizeof(key),
                                SNMP_PRIV_AES128, 1, 1, salt,
                                ciphertext, &ciphertext_len),
                    USM_OK);

        check_equal(usm_decrypt(ciphertext, ciphertext_len, key, sizeof(key),
                                SNMP_PRIV_AES128, 1, 1, salt,
                                guarded_output, &output_len),
                    USM_ERROR_INVALID);
        for (size_t index = 8; index < sizeof(guarded_output); ++index) {
            check_equal(guarded_output[index], (uint8_t)0xA5);
        }
    }
  }

  describe("USM Protocol Implementation") {
    it("should round-trip USM security parameters encoding/decoding") {
        snmp_usm_params_t params;
        uint8_t engine_id[] = {0x80, 0x00, 0x1f, 0x88, 0x80};
        params.authoritative_engine_id = engine_id;
        params.engine_id_len = sizeof(engine_id);
        params.engine_boots = 100;
        params.engine_time = 12345;
        params.user_name = "testuser";
        memset(params.auth_params, 0xAA, 12);
        memset(params.priv_params, 0xBB, 8);

        /* Encode */
        uint8_t encoded[256];
        size_t encoded_len = sizeof(encoded);

        int result = usm_encode_security_params(&params, encoded, &encoded_len);
        check_equal(result, USM_OK);
        check(encoded_len > 0);

        /* Decode with memory pool */
        MemoryPool *pool = pool_create(1024);
        snmp_usm_params_t decoded;
        result = usm_decode_security_params(encoded, encoded_len, &decoded, pool);
        check_equal(result, USM_OK);

        /* Verify */
        check_equal(decoded.engine_id_len, params.engine_id_len);
        check_equal(decoded.authoritative_engine_id, engine_id, sizeof(engine_id));
        check_equal(decoded.engine_boots, 100);
        check_equal(decoded.engine_time, 12345);
        check_equal(decoded.user_name, "testuser");
        check_equal(decoded.auth_params, params.auth_params, 12);
        check_equal(decoded.priv_params, params.priv_params, 8);

        pool_destroy(pool);
    }

    it("should handle SNMP engine time synchronization") {
        snmp_engine_time_t state;
        usm_engine_time_init(&state, 1);  /* boots = 1 */

        uint32_t boots, time_val;
        usm_engine_time_get(&state, &boots, &time_val);

        check_equal(boots, 1);
        check_equal(time_val, 0);  /* Just initialized */

        /* Update with remote time */
        usm_engine_time_update(&state, 5, 1000);

        usm_engine_time_get(&state, &boots, &time_val);
        check_equal(boots, 5);
        check(time_val >= 1000);
    }

    it("should correctly verify time windows for authentication") {
        snmp_engine_time_t local_state;
        usm_engine_time_init(&local_state, 100);
        usm_engine_time_update(&local_state, 100, 1000);

        /* Same boots, within window (+50 seconds) */
        int result = usm_verify_time_window(&local_state, 100, 1050);
        check_equal(result, USM_OK);

        /* Same boots, outside window (+200 seconds) */
        result = usm_verify_time_window(&local_state, 100, 1200);
        check_equal(result, USM_ERROR_AUTH_FAILED);

        /* Old boots */
        result = usm_verify_time_window(&local_state, 99, 1000);
        check_equal(result, USM_ERROR_AUTH_FAILED);

        /* Future boots (we're behind) - should accept */
        result = usm_verify_time_window(&local_state, 101, 500);
        check_equal(result, USM_OK);
    }

    it("should successfully create a USM user from credentials") {
        uint8_t engine_id[] = {0x80, 0x00, 0x1f, 0x88, 0x80};
        snmp_v3_user_t user;

        int result = usm_create_user(
            "myuser",
            "authpassword",
            SNMP_AUTH_MD5,
            "privpassword",
            SNMP_PRIV_DES,
            engine_id,
            sizeof(engine_id),
            &user
        );

        check_equal(result, USM_OK);
        check_equal(user.user_name, "myuser");
        check_equal(user.auth_protocol, SNMP_AUTH_MD5);
        check_equal(user.priv_protocol, SNMP_PRIV_DES);
        check_equal(user.auth_key_len, 16);  /* MD5 */
        check_equal(user.priv_key_len, 16);

        free(user.user_name);
    }
  }

  describe("SNMPv3 Message Construction") {
    it("should build v3 noAuthNoPriv messages correctly") {
        /* Debug: Test HMAC first */
        uint8_t test_key[20];
        size_t test_key_len;
        uint8_t test_msg[] = "Hello";
        uint8_t auth1[12], auth2[12];

        usm_password_to_key("testpass", SNMP_AUTH_SHA1, test_key, &test_key_len);
        usm_compute_auth(test_msg, 5, test_key, test_key_len, SNMP_AUTH_SHA1, auth1);
        usm_compute_auth(test_msg, 5, test_key, test_key_len, SNMP_AUTH_SHA1, auth2);

        check_equal(auth1, auth2, 12);  // Should be deterministic

        snmp_oid_t oid;
        snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid);

        uint8_t engine_id[] = {0x80, 0x00, 0x1f, 0x88, 0x80};
        snmp_usm_params_t usm_params = {0};
        usm_params.authoritative_engine_id = engine_id;
        usm_params.engine_id_len = sizeof(engine_id);
        usm_params.engine_boots = 0;
        usm_params.engine_time = 0;
        usm_params.user_name = "public";

        uint8_t out[1024];
        size_t out_len = sizeof(out);

        int result = snmp_build_v3_get_request(
            12345,  /* request_id */
            &oid, 1,
            &usm_params,
            NULL,  /* no user for noAuthNoPriv */
            SNMP_SEC_LEVEL_NOAUTH_NOPRIV,
            out,
            &out_len
        );

        check_equal(result, SNMP_BUILD_OK);
        check(out_len > 0);

        /* Verify it starts with SEQUENCE tag and version 3 */
        check_equal(out[0], 0x30);  /* SEQUENCE */

        snmp_oid_free(&oid);
    }

    it("encodes empty security parameters for noAuthNoPriv") {
        static uint8_t engine_id[] = {0x80, 0x00, 0x1f, 0x88, 0x80};
        snmp_usm_params_t usm_params = {0};
        snmp_oid_t oid = {0};
        uint8_t request[1024];
        size_t request_len = sizeof(request);
        size_t auth_len = SIZE_MAX;
        size_t priv_len = SIZE_MAX;

        usm_params.authoritative_engine_id = engine_id;
        usm_params.engine_id_len = sizeof(engine_id);
        usm_params.user_name = "public";
        check_equal(snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid),
                    SNMP_BUILD_OK);
        check_equal(snmp_build_v3_get_request(12345, &oid, 1, &usm_params, NULL,
                                              SNMP_SEC_LEVEL_NOAUTH_NOPRIV,
                                              request, &request_len),
                    SNMP_BUILD_OK);
        check_equal(test_usm_parameter_lengths(request, request_len,
                                               &auth_len, &priv_len), 0);
        check_equal(auth_len, (size_t)0);
        check_equal(priv_len, (size_t)0);

        snmp_oid_free(&oid);
    }

    it("should roundtrip authNoPriv messages successfully") {
        /* Create user */
        uint8_t engine_id[] = {0x80, 0x00, 0x1f, 0x88, 0x80, 0x12, 0x34};
        snmp_v3_user_t user;
        usm_create_user(
            "authuser",
            "myauthpassword",
            SNMP_AUTH_SHA1,
            NULL,  /* no priv */
            SNMP_PRIV_NONE,
            engine_id,
            sizeof(engine_id),
            &user
        );

        /* Build message */
        snmp_oid_t oid;
        snmp_oid_from_string("1.3.6.1.2.1.1.3.0", &oid);

        snmp_usm_params_t usm_params = {0};
        usm_params.authoritative_engine_id = engine_id;
        usm_params.engine_id_len = sizeof(engine_id);
        usm_params.engine_boots = 10;
        usm_params.engine_time = 5000;
        usm_params.user_name = "authuser";

        uint8_t request[1024];
        size_t request_len = sizeof(request);

        int result = snmp_build_v3_get_request(
            99999,
            &oid, 1,
            &usm_params,
            &user,
            SNMP_SEC_LEVEL_AUTH_NOPRIV,
            request,
            &request_len
        );

        check_equal(result, SNMP_BUILD_OK);
        check(request_len > 100);

        /* Parse back */
        snmp_message_t response;
        MemoryPool *pool = pool_create(8192);

        result = snmp_parse_v3(request, request_len, &response, &user, pool);

        check_equal((size_t)result, request_len);
        check_equal(response.version, SNMP_VERSION_3);
        check_equal(response.v3_header.msg_id, 99999);

        pool_destroy(pool);
        snmp_oid_free(&oid);
        free(user.user_name);
    }

    it("rejects an authenticated message whose digest was changed") {
        uint8_t request[1024];
        size_t request_len = sizeof(request);
        snmp_v3_user_t user = {0};
        snmp_oid_t oid = {0};
        snmp_message_t decoded = {0};
        MemoryPool *pool = pool_create(8192);

        check_equal(build_test_auth_request(request, &request_len, &user, &oid),
                    SNMP_BUILD_OK);
        check_not_null(pool);
        check_equal((size_t)snmp_parse_v3(request, request_len, &decoded, &user, pool),
                    request_len);

        const size_t auth_offset = find_test_bytes(request, request_len,
                                                   decoded.usm_params.auth_params, 12);
        check_not_equal(auth_offset, SIZE_MAX);
        request[auth_offset] ^= 0x01u;
        pool_destroy(pool);
        pool = pool_create(8192);

        check_equal(snmp_parse_v3(request, request_len, &decoded, &user, pool), -7);

        pool_destroy(pool);
        snmp_oid_free(&oid);
        free(user.user_name);
    }

    it("rejects an authenticated message without user credentials") {
        uint8_t request[1024];
        size_t request_len = sizeof(request);
        snmp_v3_user_t user = {0};
        snmp_oid_t oid = {0};
        snmp_message_t decoded = {0};
        MemoryPool *pool = pool_create(8192);

        check_equal(build_test_auth_request(request, &request_len, &user, &oid),
                    SNMP_BUILD_OK);
        check_not_null(pool);
        check_equal(snmp_parse_v3(request, request_len, &decoded, NULL, pool), -7);

        pool_destroy(pool);
        snmp_oid_free(&oid);
        free(user.user_name);
    }

    it("rejects an authenticated message for a different user") {
        uint8_t request[1024];
        size_t request_len = sizeof(request);
        snmp_v3_user_t user = {0};
        snmp_oid_t oid = {0};
        snmp_message_t decoded = {0};
        MemoryPool *pool = pool_create(8192);

        check_equal(build_test_auth_request(request, &request_len, &user, &oid),
                    SNMP_BUILD_OK);
        check_not_null(pool);
        user.user_name[0] = 'x';
        check_equal(snmp_parse_v3(request, request_len, &decoded, &user, pool), -7);

        pool_destroy(pool);
        snmp_oid_free(&oid);
        free(user.user_name);
    }

    it("rejects authNoPriv construction without authentication credentials") {
        static uint8_t engine_id[] = {0x80, 0x00, 0x1f, 0x88, 0x80};
        snmp_usm_params_t usm_params = {0};
        snmp_oid_t oid = {0};
        uint8_t request[1024];
        size_t request_len = sizeof(request);

        usm_params.authoritative_engine_id = engine_id;
        usm_params.engine_id_len = sizeof(engine_id);
        usm_params.user_name = "authuser";
        check_equal(snmp_oid_from_string("1.3.6.1.2.1.1.3.0", &oid),
                    SNMP_BUILD_OK);
        check_equal(snmp_build_v3_get_request(1, &oid, 1, &usm_params, NULL,
                                              SNMP_SEC_LEVEL_AUTH_NOPRIV,
                                              request, &request_len),
                    SNMP_BUILD_ERROR_INVALID);

        snmp_oid_free(&oid);
    }
  }
}
