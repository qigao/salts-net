/**
 * test_stun.c - Unit tests for STUN client
 */

#include "unity.h"
#include "ice/turbo_stun.h"
#include <string.h>
#include <string.h>



void setUp(void) {}
void tearDown(void) {}

/* ============================================================================
 * Transaction ID Tests
 * ============================================================================ */

void test_transaction_id_generation(void) {
    stun_transaction_id_t id1, id2;

    stun_generate_transaction_id(&id1);
    stun_generate_transaction_id(&id2);

    /* IDs should be different */
    TEST_ASSERT_FALSE(memcmp(id1.id, id2.id, STUN_TRANSACTION_ID_LEN) == 0);
}

void test_transaction_id_length(void) {
    stun_transaction_id_t id;
    stun_generate_transaction_id(&id);

    /* Should be 12 bytes */
    TEST_ASSERT_EQUAL(12, STUN_TRANSACTION_ID_LEN);
}

/* ============================================================================
 * STUN Message Building Tests
 * ============================================================================ */

void test_build_binding_request(void) {
    uint8_t buffer[64];
    stun_transaction_id_t txn_id;

    memset(txn_id.id, 0x42, STUN_TRANSACTION_ID_LEN);

    size_t len = stun_build_binding_request(buffer, &txn_id);

    /* Should be exactly 20 bytes (header only) */
    TEST_ASSERT_EQUAL(STUN_HEADER_SIZE, len);

    /* Message type: Binding Request (0x0001) */
    TEST_ASSERT_EQUAL(0x00, buffer[0]);
    TEST_ASSERT_EQUAL(0x01, buffer[1]);

    /* Message length: 0 */
    TEST_ASSERT_EQUAL(0x00, buffer[2]);
    TEST_ASSERT_EQUAL(0x00, buffer[3]);

    /* Magic cookie: 0x2112A442 */
    TEST_ASSERT_EQUAL(0x21, buffer[4]);
    TEST_ASSERT_EQUAL(0x12, buffer[5]);
    TEST_ASSERT_EQUAL(0xA4, buffer[6]);
    TEST_ASSERT_EQUAL(0x42, buffer[7]);

    /* Transaction ID */
    for (int i = 0; i < STUN_TRANSACTION_ID_LEN; i++) {
        TEST_ASSERT_EQUAL(0x42, buffer[8 + i]);
    }
}

void test_stun_header_size(void) {
    TEST_ASSERT_EQUAL(20, STUN_HEADER_SIZE);
}

void test_stun_magic_cookie(void) {
    TEST_ASSERT_EQUAL_HEX32(0x2112A442, STUN_MAGIC_COOKIE);
}

/* ============================================================================
 * STUN Message Detection Tests
 * ============================================================================ */

void test_is_stun_message_valid(void) {
    uint8_t buffer[20];
    stun_transaction_id_t txn_id;
    stun_generate_transaction_id(&txn_id);

    stun_build_binding_request(buffer, &txn_id);

    TEST_ASSERT_TRUE(stun_is_stun_message(buffer, 20));
}

void test_is_stun_message_too_short(void) {
    uint8_t buffer[10] = {0};
    TEST_ASSERT_FALSE(stun_is_stun_message(buffer, 10));
}

void test_is_stun_message_bad_cookie(void) {
    uint8_t buffer[20] = {0};
    /* Set first two bits to 0 but bad cookie */
    buffer[4] = 0xFF;
    buffer[5] = 0xFF;
    buffer[6] = 0xFF;
    buffer[7] = 0xFF;

    TEST_ASSERT_FALSE(stun_is_stun_message(buffer, 20));
}

void test_is_stun_message_bad_first_bits(void) {
    uint8_t buffer[20] = {0};
    /* Set magic cookie correctly */
    buffer[4] = 0x21;
    buffer[5] = 0x12;
    buffer[6] = 0xA4;
    buffer[7] = 0x42;
    /* But first two bits are not 0 */
    buffer[0] = 0xC0;

    TEST_ASSERT_FALSE(stun_is_stun_message(buffer, 20));
}

/* ============================================================================
 * STUN Response Parsing Tests
 * ============================================================================ */

void test_parse_binding_response_ipv4(void) {
    /* Simulated STUN binding response with XOR-MAPPED-ADDRESS */
    uint8_t response[] = {
        /* Header */
        0x01, 0x01,             /* Type: Binding Response */
        0x00, 0x0C,             /* Length: 12 bytes */
        0x21, 0x12, 0xA4, 0x42, /* Magic Cookie */
        0x01, 0x02, 0x03, 0x04, /* Transaction ID (12 bytes) */
        0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C,

        /* XOR-MAPPED-ADDRESS attribute */
        0x00, 0x20,             /* Type: XOR-MAPPED-ADDRESS */
        0x00, 0x08,             /* Length: 8 bytes */
        0x00, 0x01,             /* Reserved + Family (IPv4) */
        /* XOR'd port: 54321 XOR 0x2112 = 0xE5B3 XOR 0x2112 = 0xC4A1 -> actually let's compute */
        /* port = 54321 = 0xD431, XOR with 0x2112 = 0xF523 */
        0xF5, 0x23,
        /* XOR'd address: 203.0.113.1 (0xCB007101) XOR 0x2112A442 = 0xEA12D543 */
        0xEA, 0x12, 0xD5, 0x43
    };

    stun_transaction_id_t expected_txn;
    memcpy(expected_txn.id, response + 8, STUN_TRANSACTION_ID_LEN);

    stun_mapped_address_t mapped;
    int result = stun_parse_binding_response(response, sizeof(response), &expected_txn, &mapped);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(STUN_ADDR_FAMILY_IPV4, mapped.family);
    TEST_ASSERT_EQUAL(54321, mapped.port);
    /* IP should be 203.0.113.1 */
    TEST_ASSERT_EQUAL_STRING("203.0.113.1", mapped.ip_str);
}

void test_parse_binding_response_error_type(void) {
    /* STUN error response */
    uint8_t response[] = {
        0x01, 0x11,             /* Type: Binding Error Response */
        0x00, 0x00,
        0x21, 0x12, 0xA4, 0x42,
        0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C
    };

    stun_mapped_address_t mapped;
    int result = stun_parse_binding_response(response, sizeof(response), NULL, &mapped);

    TEST_ASSERT_EQUAL(-4, result); /* Error response */
}

void test_parse_binding_response_bad_cookie(void) {
    uint8_t response[] = {
        0x01, 0x01,
        0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, /* Bad cookie */
        0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C
    };

    stun_mapped_address_t mapped;
    int result = stun_parse_binding_response(response, sizeof(response), NULL, &mapped);

    TEST_ASSERT_EQUAL(-3, result); /* Bad cookie */
}

void test_parse_binding_response_too_short(void) {
    uint8_t response[10] = {0};

    stun_mapped_address_t mapped;
    int result = stun_parse_binding_response(response, sizeof(response), NULL, &mapped);

    TEST_ASSERT_EQUAL(-2, result); /* Too short */
}

/* ============================================================================
 * STUN Client State Tests
 * ============================================================================ */

void test_stun_client_create_null_config(void) {
    turbo_stun_client_t *client = stun_client_create(NULL);
    TEST_ASSERT_NULL(client);
}

void test_stun_client_create_null_loop(void) {
    stun_client_config_t config = {
        .server_host = "stun.l.google.com",
        .server_port = 19302
    };


    turbo_stun_client_t *client = stun_client_create(&config);
    TEST_ASSERT_NULL(client);
}

void test_stun_client_create_null_host(void) {
    stun_client_config_t config = {
        .server_host = NULL,
        .server_port = 19302
    };

    turbo_stun_client_t *client = stun_client_create(&config);
    TEST_ASSERT_NULL(client);
}


void test_stun_client_create_valid(void) {
    stun_client_config_t config = {
        .server_host = "stun.l.google.com",
        .server_port = 19302,
        .timeout_ms = 3000,
        .retries = 3
    };

    turbo_stun_client_t *client = stun_client_create(&config);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(STUN_CLIENT_STATE_IDLE, stun_client_get_state(client));

    stun_client_destroy(client);
}


void test_stun_client_default_values(void) {
    stun_client_config_t config = {
        .server_host = "stun.l.google.com",
        .server_port = 0,    /* Should default to 3478 */
        .timeout_ms = 0,     /* Should default to 3000 */
        .retries = 0         /* Should default to 3 */
    };

    turbo_stun_client_t *client = stun_client_create(&config);
    TEST_ASSERT_NOT_NULL(client);

    /* Check internal defaults were applied */
    TEST_ASSERT_EQUAL(STUN_DEFAULT_PORT, client->server_port);
    TEST_ASSERT_EQUAL(3000, client->timeout_ms);
    TEST_ASSERT_EQUAL(3, client->retries);

    stun_client_destroy(client);
}


/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();

    /* Transaction ID tests */
    RUN_TEST(test_transaction_id_generation);
    RUN_TEST(test_transaction_id_length);

    /* Message building tests */
    RUN_TEST(test_build_binding_request);
    RUN_TEST(test_stun_header_size);
    RUN_TEST(test_stun_magic_cookie);

    /* Message detection tests */
    RUN_TEST(test_is_stun_message_valid);
    RUN_TEST(test_is_stun_message_too_short);
    RUN_TEST(test_is_stun_message_bad_cookie);
    RUN_TEST(test_is_stun_message_bad_first_bits);

    /* Response parsing tests */
    RUN_TEST(test_parse_binding_response_ipv4);
    RUN_TEST(test_parse_binding_response_error_type);
    RUN_TEST(test_parse_binding_response_bad_cookie);
    RUN_TEST(test_parse_binding_response_too_short);

    /* Client tests */
    RUN_TEST(test_stun_client_create_null_config);
    // RUN_TEST(test_stun_client_create_null_loop); // This test is no longer valid as loop is internal
    RUN_TEST(test_stun_client_create_null_host);

    RUN_TEST(test_stun_client_create_valid);
    RUN_TEST(test_stun_client_default_values);

    return UNITY_END();
}
