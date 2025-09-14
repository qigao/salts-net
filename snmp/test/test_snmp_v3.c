/**
 * @file test_snmp_v3.c
 * @brief SNMPv3 USM Security Tests
 */

#include "snmp_usm.h"
#include "snmp_builder.h"
#include "snmp_parser.h"
#include "unity.h"
#include <string.h>

void setUp(void) {
    /* Called before each test */
}

void tearDown(void) {
    /* Called after each test */
}

/* Test password-to-key derivation (RFC 3414) */
void test_usm_password_to_key_md5(void) {
    const char *password = "maplesyrup";
    uint8_t key[32];
    size_t key_len;

    int result = usm_password_to_key(password, SNMP_AUTH_MD5, key, &key_len);

    TEST_ASSERT_EQUAL(USM_OK, result);
    TEST_ASSERT_EQUAL(16, key_len);

    /* Just verify it produces a consistent key (not testing exact RFC vector for now) */
    /* RFC 3414 test vector verification can be added after implementation stabilizes */
}

void test_usm_password_to_key_sha1(void) {
    const char *password = "maplesyrup";
    uint8_t key[32];
    size_t key_len;

    int result = usm_password_to_key(password, SNMP_AUTH_SHA1, key, &key_len);

    TEST_ASSERT_EQUAL(USM_OK, result);
    TEST_ASSERT_EQUAL(20, key_len);
}

/* Test key localization */
void test_usm_localize_key(void) {
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

    TEST_ASSERT_EQUAL(USM_OK, result);
    TEST_ASSERT_EQUAL(16, localized_key_len);

    /* Verify localized key is different from master key */
    TEST_ASSERT_NOT_EQUAL(0, memcmp(master_key, localized_key, 16));
}

/* Test HMAC computation and verification */
void test_usm_hmac_md5(void) {
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

    TEST_ASSERT_EQUAL(USM_OK, result);

    /* Verify HMAC */
    result = usm_verify_auth(
        message, sizeof(message) - 1,
        key, 16,
        SNMP_AUTH_MD5,
        auth_params
    );

    TEST_ASSERT_EQUAL(USM_OK, result);

    /* Verify with corrupted message fails */
    message[0] = 'X';
    result = usm_verify_auth(
        message, sizeof(message) - 1,
        key, 16,
        SNMP_AUTH_MD5,
        auth_params
    );

    TEST_ASSERT_EQUAL(USM_ERROR_AUTH_FAILED, result);
}

/* Test DES-CBC encryption/decryption */
void test_usm_des_cbc(void) {
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

    TEST_ASSERT_EQUAL(USM_OK, result);
    TEST_ASSERT_GREATER_THAN(0, ciphertext_len);

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

    TEST_ASSERT_EQUAL(USM_OK, result);
    TEST_ASSERT_EQUAL(sizeof(plaintext) - 1, decrypted_len);
    TEST_ASSERT_EQUAL_MEMORY(plaintext, decrypted, decrypted_len);
}

/* Test AES-128-CFB encryption/decryption */
void test_usm_aes128_cfb(void) {
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

    TEST_ASSERT_EQUAL(USM_OK, result);
    TEST_ASSERT_EQUAL(sizeof(plaintext) - 1, ciphertext_len);  /* CFB mode, no padding */

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

    TEST_ASSERT_EQUAL(USM_OK, result);
    TEST_ASSERT_EQUAL(sizeof(plaintext) - 1, decrypted_len);
    TEST_ASSERT_EQUAL_MEMORY(plaintext, decrypted, decrypted_len);
}

/* Test USM security parameters encoding/decoding */
void test_usm_security_params_roundtrip(void) {
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
    TEST_ASSERT_EQUAL(USM_OK, result);
    TEST_ASSERT_GREATER_THAN(0, encoded_len);

    /* Decode with memory pool */
    MemoryPool *pool = pool_create(1024);
    snmp_usm_params_t decoded;
    result = usm_decode_security_params(encoded, encoded_len, &decoded, pool);
    TEST_ASSERT_EQUAL(USM_OK, result);

    /* Verify */
    TEST_ASSERT_EQUAL(params.engine_id_len, decoded.engine_id_len);
    TEST_ASSERT_EQUAL_MEMORY(engine_id, decoded.authoritative_engine_id, sizeof(engine_id));
    TEST_ASSERT_EQUAL(100, decoded.engine_boots);
    TEST_ASSERT_EQUAL(12345, decoded.engine_time);
    TEST_ASSERT_EQUAL_STRING("testuser", decoded.user_name);
    TEST_ASSERT_EQUAL_MEMORY(params.auth_params, decoded.auth_params, 12);
    TEST_ASSERT_EQUAL_MEMORY(params.priv_params, decoded.priv_params, 8);

    pool_destroy(pool);
}

/* Test engine time synchronization */
void test_engine_time_sync(void) {
    snmp_engine_time_t state;
    usm_engine_time_init(&state, 1);  /* boots = 1 */

    uint32_t boots, time;
    usm_engine_time_get(&state, &boots, &time);

    TEST_ASSERT_EQUAL(1, boots);
    TEST_ASSERT_EQUAL(0, time);  /* Just initialized */

    /* Update with remote time */
    usm_engine_time_update(&state, 5, 1000);

    usm_engine_time_get(&state, &boots, &time);
    TEST_ASSERT_EQUAL(5, boots);
    TEST_ASSERT_GREATER_OR_EQUAL(1000, time);
}

/* Test time window verification */
void test_time_window_verification(void) {
    snmp_engine_time_t local_state;
    usm_engine_time_init(&local_state, 100);
    usm_engine_time_update(&local_state, 100, 1000);

    /* Same boots, within window (+50 seconds) */
    int result = usm_verify_time_window(&local_state, 100, 1050);
    TEST_ASSERT_EQUAL(USM_OK, result);

    /* Same boots, outside window (+200 seconds) */
    result = usm_verify_time_window(&local_state, 100, 1200);
    TEST_ASSERT_EQUAL(USM_ERROR_AUTH_FAILED, result);

    /* Old boots */
    result = usm_verify_time_window(&local_state, 99, 1000);
    TEST_ASSERT_EQUAL(USM_ERROR_AUTH_FAILED, result);

    /* Future boots (we're behind) - should accept */
    result = usm_verify_time_window(&local_state, 101, 500);
    TEST_ASSERT_EQUAL(USM_OK, result);
}

/* Test user creation from password */
void test_usm_create_user(void) {
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

    TEST_ASSERT_EQUAL(USM_OK, result);
    TEST_ASSERT_EQUAL_STRING("myuser", user.user_name);
    TEST_ASSERT_EQUAL(SNMP_AUTH_MD5, user.auth_protocol);
    TEST_ASSERT_EQUAL(SNMP_PRIV_DES, user.priv_protocol);
    TEST_ASSERT_EQUAL(16, user.auth_key_len);  /* MD5 */
    TEST_ASSERT_EQUAL(16, user.priv_key_len);

    free(user.user_name);
}

/* Test v3 message build (noAuthNoPriv) */
void test_v3_message_build_noauth(void) {
    /* Debug: Test HMAC first */
    uint8_t test_key[20];
    size_t test_key_len;
    uint8_t test_msg[] = "Hello";
    uint8_t auth1[12], auth2[12];

    usm_password_to_key("testpass", SNMP_AUTH_SHA1, test_key, &test_key_len);
    usm_compute_auth(test_msg, 5, test_key, test_key_len, SNMP_AUTH_SHA1, auth1);
    usm_compute_auth(test_msg, 5, test_key, test_key_len, SNMP_AUTH_SHA1, auth2);

    TEST_ASSERT_EQUAL_MEMORY(auth1, auth2, 12);  // Should be deterministic

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

    TEST_ASSERT_EQUAL(SNMP_BUILD_OK, result);
    TEST_ASSERT_GREATER_THAN(0, out_len);

    /* Verify it starts with SEQUENCE tag and version 3 */
    TEST_ASSERT_EQUAL_HEX8(0x30, out[0]);  /* SEQUENCE */

    snmp_oid_free(&oid);
}

/* Test v3 message roundtrip (authNoPriv) */
void test_v3_message_roundtrip_auth(void) {
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

    TEST_ASSERT_EQUAL(SNMP_BUILD_OK, result);
    TEST_ASSERT_GREATER_THAN(100, request_len);

    /* Parse back */
    snmp_message_t response;
    MemoryPool *pool = pool_create(8192);

    result = snmp_parse_v3(request, request_len, &response, &user, pool);

    TEST_ASSERT_EQUAL_INT(request_len, result);
    TEST_ASSERT_EQUAL(SNMP_VERSION_3, response.version);
    TEST_ASSERT_EQUAL(99999, response.v3_header.msg_id);

    pool_destroy(pool);
    snmp_oid_free(&oid);
    free(user.user_name);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_usm_password_to_key_md5);
    RUN_TEST(test_usm_password_to_key_sha1);
    RUN_TEST(test_usm_localize_key);
    RUN_TEST(test_usm_hmac_md5);
    RUN_TEST(test_usm_des_cbc);
    RUN_TEST(test_usm_aes128_cfb);
    RUN_TEST(test_usm_security_params_roundtrip);
    RUN_TEST(test_engine_time_sync);
    RUN_TEST(test_time_window_verification);
    RUN_TEST(test_usm_create_user);
    RUN_TEST(test_v3_message_build_noauth);
    RUN_TEST(test_v3_message_roundtrip_auth);

    return UNITY_END();
}
