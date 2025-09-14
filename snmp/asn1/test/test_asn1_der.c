/**
 * @file test_asn1_der.c
 * @brief ASN.1 DER Encoder/Decoder Tests
 */

#include "unity.h"
#include "asn1_types.h"
#include <string.h>
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

/* ============================================================================
 * ASN.1 Value Creation Tests
 * ============================================================================ */

void test_create_boolean(void) {
    asn1_value_t *v = asn1_create_boolean(1);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_BOOLEAN, v->type);
    TEST_ASSERT_EQUAL(1, v->value.boolean);
    TEST_ASSERT_EQUAL_HEX8(0x01, v->tag); // BOOLEAN tag
    asn1_free(v);
}

void test_create_integer(void) {
    asn1_value_t *v = asn1_create_integer(42);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_INTEGER, v->type);
    TEST_ASSERT_EQUAL(42, v->value.integer);
    TEST_ASSERT_EQUAL_HEX8(0x02, v->tag); // INTEGER tag
    asn1_free(v);
}

void test_create_octet_string(void) {
    uint8_t data[] = {0x01, 0x02, 0x03};
    asn1_value_t *v = asn1_create_octet_string(data, sizeof(data));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_OCTET_STRING, v->type);
    TEST_ASSERT_EQUAL(3, v->value.octet_string.length);
    TEST_ASSERT_EQUAL_MEMORY(data, v->value.octet_string.data, 3);
    TEST_ASSERT_EQUAL_HEX8(0x04, v->tag); // OCTET STRING tag
    asn1_free(v);
}

void test_create_null(void) {
    asn1_value_t *v = asn1_create_null();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_NULL, v->type);
    TEST_ASSERT_EQUAL_HEX8(0x05, v->tag); // NULL tag
    asn1_free(v);
}

void test_create_sequence(void) {
    asn1_value_t *seq = asn1_create_sequence();
    TEST_ASSERT_NOT_NULL(seq);
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, seq->type);
    TEST_ASSERT_EQUAL_HEX8(0x30, seq->tag); // SEQUENCE tag (constructed)

    asn1_value_t *child = asn1_create_integer(123);
    TEST_ASSERT_EQUAL(0, asn1_sequence_add_child(seq, child));
    TEST_ASSERT_EQUAL(1, seq->value.sequence.count);

    asn1_free(seq);
}

void test_create_set(void) {
    asn1_value_t *set = asn1_create_set();
    TEST_ASSERT_NOT_NULL(set);
    TEST_ASSERT_EQUAL(ASN1_TYPE_SET, set->type);
    TEST_ASSERT_EQUAL_HEX8(0x31, set->tag); // SET tag (constructed)

    asn1_value_t *child = asn1_create_boolean(1);
    TEST_ASSERT_EQUAL(0, asn1_set_add_child(set, child));
    TEST_ASSERT_EQUAL(1, set->value.set.count);

    asn1_free(set);
}

/* ============================================================================
 * String Type Creation Tests
 * ============================================================================ */

void test_create_bit_string(void) {
    uint8_t data[] = {0xAB, 0xCD};
    asn1_value_t *v = asn1_create_bit_string(data, 2, 4);  // 4 unused bits
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_BIT_STRING, v->type);
    TEST_ASSERT_EQUAL_HEX8(0x03, v->tag); // BIT STRING tag
    // First byte should be unused bits count
    TEST_ASSERT_EQUAL(4, v->value.octet_string.data[0]);
    TEST_ASSERT_EQUAL(0xAB, v->value.octet_string.data[1]);
    TEST_ASSERT_EQUAL(0xCD, v->value.octet_string.data[2]);
    asn1_free(v);
}

void test_create_oid_from_string(void) {
    asn1_value_t *v = asn1_create_oid_from_string("1.2.840.113549");
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_OBJECT_IDENTIFIER, v->type);
    TEST_ASSERT_EQUAL_HEX8(0x06, v->tag); // OID tag
    TEST_ASSERT_EQUAL(4, v->value.oid.count);
    TEST_ASSERT_EQUAL(1, v->value.oid.components[0]);
    TEST_ASSERT_EQUAL(2, v->value.oid.components[1]);
    TEST_ASSERT_EQUAL(840, v->value.oid.components[2]);
    TEST_ASSERT_EQUAL(113549, v->value.oid.components[3]);
    asn1_free(v);
}

void test_create_printable_string(void) {
    asn1_value_t *v = asn1_create_printable_string("Hello World");
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_PRINTABLE_STRING, v->type);
    TEST_ASSERT_EQUAL_HEX8(0x13, v->tag); // PrintableString tag
    TEST_ASSERT_EQUAL(11, v->value.octet_string.length);
    TEST_ASSERT_EQUAL_MEMORY("Hello World", v->value.octet_string.data, 11);
    asn1_free(v);
}

void test_create_utf8_string(void) {
    asn1_value_t *v = asn1_create_utf8_string("UTF8");
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_UTF8_STRING, v->type);
    TEST_ASSERT_EQUAL_HEX8(0x0C, v->tag); // UTF8String tag
    TEST_ASSERT_EQUAL(4, v->value.octet_string.length);
    TEST_ASSERT_EQUAL_MEMORY("UTF8", v->value.octet_string.data, 4);
    asn1_free(v);
}

void test_create_ia5_string(void) {
    asn1_value_t *v = asn1_create_ia5_string("test@example.com");
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_IA5_STRING, v->type);
    TEST_ASSERT_EQUAL_HEX8(0x16, v->tag); // IA5String tag
    TEST_ASSERT_EQUAL(16, v->value.octet_string.length);
    TEST_ASSERT_EQUAL_MEMORY("test@example.com", v->value.octet_string.data, 16);
    asn1_free(v);
}

void test_create_utc_time(void) {
    asn1_value_t *v = asn1_create_utc_time("231231235959Z");
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_UTC_TIME, v->type);
    TEST_ASSERT_EQUAL_HEX8(0x17, v->tag); // UTCTime tag
    TEST_ASSERT_EQUAL(13, v->value.octet_string.length);
    TEST_ASSERT_EQUAL_MEMORY("231231235959Z", v->value.octet_string.data, 13);
    asn1_free(v);
}

/* ============================================================================
 * Binary Parsing Tests
 * ============================================================================ */

void test_parse_boolean_true(void) {
    uint8_t data[] = {0x01, 0x01, 0xFF};  // BOOLEAN TRUE
    asn1_value_t *v = NULL;
    
    TEST_ASSERT_EQUAL(0, scan_binary_asn1(data, sizeof(data), &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_BOOLEAN, v->type);
    TEST_ASSERT_TRUE(v->value.boolean);
    
    asn1_free(v);
}

void test_parse_boolean_false(void) {
    uint8_t data[] = {0x01, 0x01, 0x00};  // BOOLEAN FALSE
    asn1_value_t *v = NULL;
    
    TEST_ASSERT_EQUAL(0, scan_binary_asn1(data, sizeof(data), &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_BOOLEAN, v->type);
    TEST_ASSERT_FALSE(v->value.boolean);
    
    asn1_free(v);
}

void test_parse_integer(void) {
    uint8_t data[] = {0x02, 0x01, 0x2A};  // INTEGER 42
    asn1_value_t *v = NULL;
    
    TEST_ASSERT_EQUAL(0, scan_binary_asn1(data, sizeof(data), &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_INTEGER, v->type);
    TEST_ASSERT_EQUAL(42, v->value.integer);
    
    asn1_free(v);
}

void test_parse_octet_string(void) {
    uint8_t data[] = {0x04, 0x03, 0xAB, 0xCD, 0xEF};  // OCTET STRING
    asn1_value_t *v = NULL;
    
    TEST_ASSERT_EQUAL(0, scan_binary_asn1(data, sizeof(data), &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_OCTET_STRING, v->type);
    TEST_ASSERT_EQUAL(3, v->value.octet_string.length);
    TEST_ASSERT_EQUAL(0xAB, v->value.octet_string.data[0]);
    TEST_ASSERT_EQUAL(0xCD, v->value.octet_string.data[1]);
    TEST_ASSERT_EQUAL(0xEF, v->value.octet_string.data[2]);
    
    asn1_free(v);
}

void test_parse_null(void) {
    uint8_t data[] = {0x05, 0x00};  // NULL
    asn1_value_t *v = NULL;
    
    TEST_ASSERT_EQUAL(0, scan_binary_asn1(data, sizeof(data), &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_NULL, v->type);
    
    asn1_free(v);
}

void test_parse_sequence(void) {
    // SEQUENCE { INTEGER 42, BOOLEAN TRUE }
    // 30 06 02 01 2A 01 01 FF
    uint8_t data[] = {0x30, 0x06, 0x02, 0x01, 0x2A, 0x01, 0x01, 0xFF};
    asn1_value_t *v = NULL;
    
    TEST_ASSERT_EQUAL(0, scan_binary_asn1(data, sizeof(data), &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, v->type);
    TEST_ASSERT_EQUAL(2, v->value.sequence.count);

    TEST_ASSERT_EQUAL(ASN1_TYPE_INTEGER, v->value.sequence.children[0]->type);
    TEST_ASSERT_EQUAL(42, v->value.sequence.children[0]->value.integer);

    TEST_ASSERT_EQUAL(ASN1_TYPE_BOOLEAN, v->value.sequence.children[1]->type);
    TEST_ASSERT_TRUE(v->value.sequence.children[1]->value.boolean);
    
    asn1_free(v);
}

/* ============================================================================
 * Complex Structure Tests
 * ============================================================================ */

void test_nested_sequence(void) {
    // Create nested structure: SEQUENCE { SEQUENCE { INTEGER 1 }, BOOLEAN TRUE }
    asn1_value_t *outer = asn1_create_sequence();
    asn1_value_t *inner = asn1_create_sequence();
    
    asn1_sequence_add_child(inner, asn1_create_integer(1));
    asn1_sequence_add_child(outer, inner);
    asn1_sequence_add_child(outer, asn1_create_boolean(1));
    
    TEST_ASSERT_EQUAL(2, outer->value.sequence.count);
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, outer->value.sequence.children[0]->type);
    TEST_ASSERT_EQUAL(1, outer->value.sequence.children[0]->value.sequence.count);
    TEST_ASSERT_EQUAL(ASN1_TYPE_BOOLEAN, outer->value.sequence.children[1]->type);
    
    asn1_free(outer);
}

void test_mixed_types_sequence(void) {
    asn1_value_t *seq = asn1_create_sequence();
    
    asn1_sequence_add_child(seq, asn1_create_integer(123));
    asn1_sequence_add_child(seq, asn1_create_boolean(0));
    asn1_sequence_add_child(seq, asn1_create_null());
    
    uint8_t data[] = {0x01, 0x02, 0x03};
    asn1_sequence_add_child(seq, asn1_create_octet_string(data, 3));
    
    TEST_ASSERT_EQUAL(4, seq->value.sequence.count);
    TEST_ASSERT_EQUAL(ASN1_TYPE_INTEGER, seq->value.sequence.children[0]->type);
    TEST_ASSERT_EQUAL(ASN1_TYPE_BOOLEAN, seq->value.sequence.children[1]->type);
    TEST_ASSERT_EQUAL(ASN1_TYPE_NULL, seq->value.sequence.children[2]->type);
    TEST_ASSERT_EQUAL(ASN1_TYPE_OCTET_STRING, seq->value.sequence.children[3]->type);
    
    asn1_free(seq);
}

/* ============================================================================
 * Error Handling Tests
 * ============================================================================ */

void test_parse_invalid_data(void) {
    uint8_t data[] = {0xFF, 0xFF};  // Invalid tag
    asn1_value_t *v = NULL;
    
    // Should fail gracefully
    int result = scan_binary_asn1(data, sizeof(data), &v);
    TEST_ASSERT_NOT_EQUAL(0, result);
    TEST_ASSERT_NULL(v);
}

void test_parse_truncated_data(void) {
    uint8_t data[] = {0x02, 0x05};  // INTEGER with length 5 but no data
    asn1_value_t *v = NULL;
    
    // Should fail gracefully
    int result = scan_binary_asn1(data, sizeof(data), &v);
    TEST_ASSERT_NOT_EQUAL(0, result);
    TEST_ASSERT_NULL(v);
}

/* ============================================================================
 * Utility Function Tests
 * ============================================================================ */

void test_oid_comparison(void) {
    asn1_value_t *oid1 = asn1_create_oid_from_string("1.2.3.4");
    asn1_value_t *oid2 = asn1_create_oid_from_string("1.2.3.4");
    asn1_value_t *oid3 = asn1_create_oid_from_string("1.2.3.5");
    
    TEST_ASSERT_EQUAL(0, asn1_compare_oid(&oid1->value.oid, &oid2->value.oid));
    TEST_ASSERT_NOT_EQUAL(0, asn1_compare_oid(&oid1->value.oid, &oid3->value.oid));
    
    asn1_free(oid1);
    asn1_free(oid2);
    asn1_free(oid3);
}

void test_oid_to_string(void) {
    asn1_value_t *oid = asn1_create_oid_from_string("2.5.4.3");
    char *str = asn1_oid_to_string(&oid->value.oid);
    
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_EQUAL_STRING("2.5.4.3", str);
    
    free(str);
    asn1_free(oid);
}

/* ============================================================================
 * Main Test Runner
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();

    // Value creation tests
    RUN_TEST(test_create_boolean);
    RUN_TEST(test_create_integer);
    RUN_TEST(test_create_octet_string);
    RUN_TEST(test_create_null);
    RUN_TEST(test_create_sequence);
    RUN_TEST(test_create_set);

    // String type tests
    RUN_TEST(test_create_bit_string);
    RUN_TEST(test_create_oid_from_string);
    RUN_TEST(test_create_printable_string);
    RUN_TEST(test_create_utf8_string);
    RUN_TEST(test_create_ia5_string);
    RUN_TEST(test_create_utc_time);

    // Binary parsing tests
    RUN_TEST(test_parse_boolean_true);
    RUN_TEST(test_parse_boolean_false);
    RUN_TEST(test_parse_integer);
    RUN_TEST(test_parse_octet_string);
    RUN_TEST(test_parse_null);
    RUN_TEST(test_parse_sequence);

    // Complex structure tests
    RUN_TEST(test_nested_sequence);
    RUN_TEST(test_mixed_types_sequence);

    // Error handling tests
    RUN_TEST(test_parse_invalid_data);
    RUN_TEST(test_parse_truncated_data);

    // Utility function tests
    RUN_TEST(test_oid_comparison);
    RUN_TEST(test_oid_to_string);

    return UNITY_END();
}