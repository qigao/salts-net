/**
 * @file test_snmp.c
 * @brief SNMP Parser/Builder Tests
 */

#include "unity.h"
#include "snmp_parser.h"
#include "snmp_builder.h"
#include "memory_pool.h"
#include <string.h>
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

/* ============================================================================
 * OID Helper Tests
 * ============================================================================ */

void test_oid_from_string(void) {
    snmp_oid_t oid;

    /* Test sysDescr.0 = 1.3.6.1.2.1.1.1.0 (9 components) */
    int result = snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(9, oid.count);
    TEST_ASSERT_EQUAL(1, oid.components[0]);
    TEST_ASSERT_EQUAL(3, oid.components[1]);
    TEST_ASSERT_EQUAL(6, oid.components[2]);
    TEST_ASSERT_EQUAL(1, oid.components[3]);
    TEST_ASSERT_EQUAL(2, oid.components[4]);
    TEST_ASSERT_EQUAL(1, oid.components[5]);
    TEST_ASSERT_EQUAL(1, oid.components[6]);
    TEST_ASSERT_EQUAL(1, oid.components[7]);
    TEST_ASSERT_EQUAL(0, oid.components[8]);

    snmp_oid_free(&oid);
}

void test_oid_to_string(void) {
    snmp_oid_t oid;
    snmp_oid_from_string("1.3.6.1.2.1.1.5.0", &oid);

    char buf[128];
    int result = snmp_oid_to_string(&oid, buf, sizeof(buf));

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.5.0", buf);

    snmp_oid_free(&oid);
}

void test_oid_compare(void) {
    snmp_oid_t oid1, oid2, oid3;

    snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid1);
    snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid2);  /* Same as oid1 */
    snmp_oid_from_string("1.3.6.1.2.1.1.5.0", &oid3);  /* Greater than oid1 */

    TEST_ASSERT_EQUAL(0, snmp_oid_compare(&oid1, &oid2));   /* Equal */
    TEST_ASSERT_TRUE(snmp_oid_compare(&oid1, &oid3) < 0);   /* oid1 < oid3 */
    TEST_ASSERT_TRUE(snmp_oid_compare(&oid3, &oid1) > 0);   /* oid3 > oid1 */

    snmp_oid_free(&oid1);
    snmp_oid_free(&oid2);
    snmp_oid_free(&oid3);
}

/* ============================================================================
 * SNMP Builder Tests
 * ============================================================================ */

void test_build_get_request(void) {
    snmp_oid_t oid;
    snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid);  /* sysDescr.0 */

    uint8_t packet[256];
    size_t packet_len = sizeof(packet);

    int result = snmp_build_get_request(
        SNMP_VERSION_2C,
        "public",
        1234,
        &oid,
        1,
        packet,
        &packet_len
    );

    TEST_ASSERT_EQUAL(SNMP_BUILD_OK, result);
    TEST_ASSERT_TRUE(packet_len > 0);
    TEST_ASSERT_TRUE(packet_len < sizeof(packet));

    /* Verify packet starts with SEQUENCE tag */
    TEST_ASSERT_EQUAL(0x30, packet[0]);

    snmp_oid_free(&oid);
}

void test_build_get_request_multiple_oids(void) {
    snmp_oid_t oids[2];
    snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oids[0]);  /* sysDescr */
    snmp_oid_from_string("1.3.6.1.2.1.1.5.0", &oids[1]);  /* sysName */

    uint8_t packet[512];
    size_t packet_len = sizeof(packet);

    int result = snmp_build_get_request(
        SNMP_VERSION_2C,
        "public",
        5678,
        oids,
        2,
        packet,
        &packet_len
    );

    TEST_ASSERT_EQUAL(SNMP_BUILD_OK, result);
    TEST_ASSERT_TRUE(packet_len > 0);

    snmp_oid_free(&oids[0]);
    snmp_oid_free(&oids[1]);
}

/* ============================================================================
 * SNMP Parser Tests (Round-trip)
 * ============================================================================ */

void test_roundtrip_get_request(void) {
    /* Build GetRequest */
    snmp_oid_t oid;
    snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid);

    uint8_t packet[256];
    size_t packet_len = sizeof(packet);

    int build_result = snmp_build_get_request(
        SNMP_VERSION_2C,
        "public",
        1234,
        &oid,
        1,
        packet,
        &packet_len
    );

    TEST_ASSERT_EQUAL(SNMP_BUILD_OK, build_result);

    /* Parse it back */
    MemoryPool *pool = pool_create(4096);
    TEST_ASSERT_NOT_NULL(pool);

    snmp_message_t msg;
    int parse_result = snmp_parse(packet, packet_len, &msg, pool);

    TEST_ASSERT_TRUE(parse_result > 0);
    TEST_ASSERT_EQUAL(SNMP_VERSION_2C, msg.version);
    TEST_ASSERT_EQUAL_STRING_LEN("public", msg.community, msg.community_len);
    TEST_ASSERT_EQUAL(SNMP_PDU_GET_REQUEST, msg.pdu.type);
    TEST_ASSERT_EQUAL(1234, msg.pdu.request_id);
    TEST_ASSERT_EQUAL(0, msg.pdu.error_status);
    TEST_ASSERT_EQUAL(0, msg.pdu.error_index);
    TEST_ASSERT_EQUAL(1, msg.pdu.varbind_count);

    /* Verify OID */
    TEST_ASSERT_EQUAL(oid.count, msg.pdu.varbinds[0].oid.count);
    for (size_t i = 0; i < oid.count; i++) {
        TEST_ASSERT_EQUAL(oid.components[i], msg.pdu.varbinds[0].oid.components[i]);
    }

    /* Verify value is NULL */
    TEST_ASSERT_EQUAL(SNMP_TYPE_NULL, msg.pdu.varbinds[0].value_type);

    pool_destroy(pool);
    snmp_oid_free(&oid);
}

void test_roundtrip_get_next_request(void) {
    /* Build GetNextRequest */
    snmp_oid_t oid;
    snmp_oid_from_string("1.3.6.1.2.1.1", &oid);

    uint8_t packet[256];
    size_t packet_len = sizeof(packet);

    int build_result = snmp_build_get_next_request(
        SNMP_VERSION_1,
        "private",
        9999,
        &oid,
        1,
        packet,
        &packet_len
    );

    TEST_ASSERT_EQUAL(SNMP_BUILD_OK, build_result);

    /* Parse it back */
    MemoryPool *pool = pool_create(4096);
    snmp_message_t msg;
    int parse_result = snmp_parse(packet, packet_len, &msg, pool);

    TEST_ASSERT_TRUE(parse_result > 0);
    TEST_ASSERT_EQUAL(SNMP_VERSION_1, msg.version);
    TEST_ASSERT_EQUAL(SNMP_PDU_GET_NEXT_REQUEST, msg.pdu.type);
    TEST_ASSERT_EQUAL(9999, msg.pdu.request_id);

    pool_destroy(pool);
    snmp_oid_free(&oid);
}

/* ============================================================================
 * Real SNMP Packet Tests
 * ============================================================================ */

void test_parse_real_snmpv2c_get_response(void) {
    /*
     * Real SNMPv2c GetResponse packet (captured from Wireshark)
     *
     * Message:
     *   version: 1 (v2c)
     *   community: "public"
     *   PDU type: GetResponse (0xA2)
     *   request-id: 123456
     *   error-status: 0
     *   error-index: 0
     *   variable-bindings:
     *     [0] OID: 1.3.6.1.2.1.1.1.0 (sysDescr.0)
     *         value: OCTET STRING "Linux 5.4.0 x86_64" (19 bytes)
     */
    uint8_t packet[] = {
        0x30, 0x3B,                                      /* SEQUENCE, length 59 */
            0x02, 0x01, 0x01,                            /* INTEGER version = 1 (v2c) */
            0x04, 0x06, 'p', 'u', 'b', 'l', 'i', 'c',   /* OCTET STRING "public" */
            0xA2, 0x2E,                                  /* GetResponse [2], length 46 */
                0x02, 0x03, 0x01, 0xE2, 0x40,            /* INTEGER request-id = 123456 */
                0x02, 0x01, 0x00,                        /* INTEGER error-status = 0 */
                0x02, 0x01, 0x00,                        /* INTEGER error-index = 0 */
                0x30, 0x21,                              /* SEQUENCE varbinds, length 33 */
                    0x30, 0x1F,                          /* SEQUENCE varbind, length 31 */
                        0x06, 0x08, 0x2B, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00,  /* OID 1.3.6.1.2.1.1.1.0 */
                        0x04, 0x13, 'L', 'i', 'n', 'u', 'x', ' ', '5', '.', '4', '.', '0',
                                    ' ', 'x', '8', '6', '_', '6', '4', 0x00  /* OCTET STRING "Linux 5.4.0 x86_64\0" = 19 bytes */
    };

    MemoryPool *pool = pool_create(8192);
    TEST_ASSERT_NOT_NULL(pool);

    snmp_message_t msg;
    int result = snmp_parse(packet, sizeof(packet), &msg, pool);

    /* Debug: Print result if failed */
    if (result <= 0) {
        printf("\nsnmp_parse failed with error: %d\n", result);
        printf("Expected: SNMP_PARSE_OK (0) or positive bytes consumed\n");
    }

    TEST_ASSERT_TRUE(result > 0);
    TEST_ASSERT_EQUAL(SNMP_VERSION_2C, msg.version);
    TEST_ASSERT_EQUAL(6, msg.community_len);
    TEST_ASSERT_EQUAL_STRING_LEN("public", msg.community, 6);
    TEST_ASSERT_EQUAL(SNMP_PDU_GET_RESPONSE, msg.pdu.type);
    TEST_ASSERT_EQUAL(123456, msg.pdu.request_id);
    TEST_ASSERT_EQUAL(0, msg.pdu.error_status);
    TEST_ASSERT_EQUAL(0, msg.pdu.error_index);
    TEST_ASSERT_EQUAL(1, msg.pdu.varbind_count);

    /* Verify OID */
    snmp_oid_t *oid = &msg.pdu.varbinds[0].oid;
    TEST_ASSERT_EQUAL(9, oid->count);  /* 1.3.6.1.2.1.1.1.0 = 9 components */
    TEST_ASSERT_EQUAL(1, oid->components[0]);
    TEST_ASSERT_EQUAL(3, oid->components[1]);
    TEST_ASSERT_EQUAL(6, oid->components[2]);
    TEST_ASSERT_EQUAL(1, oid->components[3]);
    TEST_ASSERT_EQUAL(2, oid->components[4]);
    TEST_ASSERT_EQUAL(1, oid->components[5]);
    TEST_ASSERT_EQUAL(1, oid->components[6]);
    TEST_ASSERT_EQUAL(1, oid->components[7]);
    TEST_ASSERT_EQUAL(0, oid->components[8]);

    /* Verify value */
    TEST_ASSERT_EQUAL(SNMP_TYPE_OCTET_STRING, msg.pdu.varbinds[0].value_type);
    TEST_ASSERT_EQUAL(19, msg.pdu.varbinds[0].value.bytes.len);
    TEST_ASSERT_EQUAL_STRING_LEN("Linux 5.4.0 x86_64",
                                  (char *)msg.pdu.varbinds[0].value.bytes.data, 18);

    pool_destroy(pool);
}

/* ============================================================================
 * Test Runner
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();

    /* OID tests */
    RUN_TEST(test_oid_from_string);
    RUN_TEST(test_oid_to_string);
    RUN_TEST(test_oid_compare);

    /* Builder tests */
    RUN_TEST(test_build_get_request);
    RUN_TEST(test_build_get_request_multiple_oids);

    /* Round-trip tests */
    RUN_TEST(test_roundtrip_get_request);
    RUN_TEST(test_roundtrip_get_next_request);

    /* Real packet tests */
    RUN_TEST(test_parse_real_snmpv2c_get_response);

    return UNITY_END();
}
