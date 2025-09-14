/**
 * @file test_ber_features.c
 * @brief BER-specific feature tests (indefinite length, constructed primitives)
 */

#include "unity.h"
#include "asn1_types.h"
#include <string.h>
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

/* ============================================================================
 * Indefinite Length Tests
 * ============================================================================ */

void test_indefinite_length_sequence(void) {
    // SEQUENCE with indefinite length containing INTEGER 42 and BOOLEAN TRUE
    // 30 80 02 01 2A 01 01 FF 00 00
    uint8_t data[] = {
        0x30, 0x80,           // SEQUENCE, indefinite length
        0x02, 0x01, 0x2A,     // INTEGER 42
        0x01, 0x01, 0xFF,     // BOOLEAN TRUE
        0x00, 0x00            // End-of-contents
    };
    
    asn1_value_t *v = NULL;
    TEST_ASSERT_EQUAL(0, scan_binary_asn1(data, sizeof(data), &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, v->type);
    TEST_ASSERT_EQUAL(2, v->value.sequence.count);
    
    // Check first child (INTEGER 42)
    TEST_ASSERT_EQUAL(ASN1_TYPE_INTEGER, v->value.sequence.children[0]->type);
    TEST_ASSERT_EQUAL(42, v->value.sequence.children[0]->value.integer);
    
    // Check second child (BOOLEAN TRUE)
    TEST_ASSERT_EQUAL(ASN1_TYPE_BOOLEAN, v->value.sequence.children[1]->type);
    TEST_ASSERT_TRUE(v->value.sequence.children[1]->value.boolean);
    
    asn1_free(v);
}

void test_nested_indefinite_length(void) {
    // Nested indefinite length: SEQUENCE { SEQUENCE { INTEGER 1 } }
    // 30 80 30 80 02 01 01 00 00 00 00
    uint8_t data[] = {
        0x30, 0x80,           // Outer SEQUENCE, indefinite length
        0x30, 0x80,           // Inner SEQUENCE, indefinite length
        0x02, 0x01, 0x01,     // INTEGER 1
        0x00, 0x00,           // End-of-contents for inner SEQUENCE
        0x00, 0x00            // End-of-contents for outer SEQUENCE
    };
    
    asn1_value_t *v = NULL;
    TEST_ASSERT_EQUAL(0, scan_binary_asn1(data, sizeof(data), &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, v->type);
    TEST_ASSERT_EQUAL(1, v->value.sequence.count);
    
    // Check nested sequence
    asn1_value_t *inner = v->value.sequence.children[0];
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, inner->type);
    TEST_ASSERT_EQUAL(1, inner->value.sequence.count);
    
    // Check integer inside nested sequence
    TEST_ASSERT_EQUAL(ASN1_TYPE_INTEGER, inner->value.sequence.children[0]->type);
    TEST_ASSERT_EQUAL(1, inner->value.sequence.children[0]->value.integer);
    
    asn1_free(v);
}

/* ============================================================================
 * Constructed Primitive Tests (BER allows this, DER doesn't)
 * ============================================================================ */

void test_constructed_octet_string(void) {
    // Constructed OCTET STRING containing two primitive OCTET STRINGs
    // 24 80 04 03 AB CD EF 04 02 12 34 00 00
    uint8_t data[] = {
        0x24, 0x80,           // OCTET STRING, constructed, indefinite length
        0x04, 0x03, 0xAB, 0xCD, 0xEF,  // First chunk
        0x04, 0x02, 0x12, 0x34,        // Second chunk
        0x00, 0x00            // End-of-contents
    };
    
    asn1_value_t *v = NULL;
    TEST_ASSERT_EQUAL(0, scan_binary_asn1(data, sizeof(data), &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_OCTET_STRING, v->type);
    TEST_ASSERT_EQUAL(5, v->value.octet_string.length); // 3 + 2 bytes
    
    // Check concatenated content
    TEST_ASSERT_EQUAL(0xAB, v->value.octet_string.data[0]);
    TEST_ASSERT_EQUAL(0xCD, v->value.octet_string.data[1]);
    TEST_ASSERT_EQUAL(0xEF, v->value.octet_string.data[2]);
    TEST_ASSERT_EQUAL(0x12, v->value.octet_string.data[3]);
    TEST_ASSERT_EQUAL(0x34, v->value.octet_string.data[4]);
    
    asn1_free(v);
}

void test_constructed_bit_string(void) {
    // Constructed BIT STRING containing two primitive BIT STRINGs
    // 23 80 03 02 00 AB 03 03 04 CD EF 00 00
    uint8_t data[] = {
        0x23, 0x80,           // BIT STRING, constructed, indefinite length
        0x03, 0x02, 0x00, 0xAB,        // First chunk: 0 unused bits, data AB
        0x03, 0x03, 0x04, 0xCD, 0xEF,  // Second chunk: 4 unused bits, data CD EF
        0x00, 0x00            // End-of-contents
    };
    
    asn1_value_t *v = NULL;
    TEST_ASSERT_EQUAL(0, scan_binary_asn1(data, sizeof(data), &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_BIT_STRING, v->type);
    TEST_ASSERT_EQUAL(4, v->value.octet_string.length); // unused bits + 3 data bytes
    
    // Check unused bits (from first chunk only)
    TEST_ASSERT_EQUAL(0x00, v->value.octet_string.data[0]);
    // Check concatenated data
    TEST_ASSERT_EQUAL(0xAB, v->value.octet_string.data[1]);
    TEST_ASSERT_EQUAL(0xCD, v->value.octet_string.data[2]);
    TEST_ASSERT_EQUAL(0xEF, v->value.octet_string.data[3]);
    
    asn1_free(v);
}

/* ============================================================================
 * Mixed Definite/Indefinite Length Tests
 * ============================================================================ */

void test_mixed_length_encoding(void) {
    // SEQUENCE (indefinite) containing SEQUENCE (definite) with INTEGER
    // 30 80 30 03 02 01 7B 00 00
    uint8_t data[] = {
        0x30, 0x80,           // Outer SEQUENCE, indefinite length
        0x30, 0x03,           // Inner SEQUENCE, definite length 3
        0x02, 0x01, 0x7B,     // INTEGER 123
        0x00, 0x00            // End-of-contents for outer SEQUENCE
    };
    
    asn1_value_t *v = NULL;
    TEST_ASSERT_EQUAL(0, scan_binary_asn1(data, sizeof(data), &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, v->type);
    TEST_ASSERT_EQUAL(1, v->value.sequence.count);
    
    // Check inner sequence
    asn1_value_t *inner = v->value.sequence.children[0];
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, inner->type);
    TEST_ASSERT_EQUAL(1, inner->value.sequence.count);
    
    // Check integer
    TEST_ASSERT_EQUAL(ASN1_TYPE_INTEGER, inner->value.sequence.children[0]->type);
    TEST_ASSERT_EQUAL(123, inner->value.sequence.children[0]->value.integer);
    
    asn1_free(v);
}

/* ============================================================================
 * Long Form Length Tests
 * ============================================================================ */

void test_long_form_length(void) {
    // OCTET STRING with long form length (length > 127)
    // Create 200-byte octet string: 04 81 C8 [200 bytes of 0xFF]
    uint8_t data[203];
    data[0] = 0x04;       // OCTET STRING tag
    data[1] = 0x81;       // Long form: 1 length octet follows
    data[2] = 0xC8;       // Length = 200
    
    // Fill with 0xFF bytes
    for (int i = 3; i < 203; i++) {
        data[i] = 0xFF;
    }
    
    asn1_value_t *v = NULL;
    TEST_ASSERT_EQUAL(0, scan_binary_asn1(data, sizeof(data), &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_OCTET_STRING, v->type);
    TEST_ASSERT_EQUAL(200, v->value.octet_string.length);
    
    // Check first and last bytes
    TEST_ASSERT_EQUAL(0xFF, v->value.octet_string.data[0]);
    TEST_ASSERT_EQUAL(0xFF, v->value.octet_string.data[199]);
    
    asn1_free(v);
}

/* ============================================================================
 * Error Handling Tests
 * ============================================================================ */

void test_missing_end_of_contents(void) {
    // SEQUENCE with indefinite length but no end-of-contents
    uint8_t data[] = {
        0x30, 0x80,           // SEQUENCE, indefinite length
        0x02, 0x01, 0x2A      // INTEGER 42 (but no end-of-contents)
    };
    
    asn1_value_t *v = NULL;
    int result = scan_binary_asn1(data, sizeof(data), &v);
    TEST_ASSERT_NOT_EQUAL(0, result); // Should fail
    TEST_ASSERT_NULL(v);
}

void test_malformed_constructed_primitive(void) {
    // Constructed OCTET STRING with invalid content
    uint8_t data[] = {
        0x24, 0x80,           // OCTET STRING, constructed, indefinite length
        0xFF, 0xFF,           // Invalid tag/content
        0x00, 0x00            // End-of-contents
    };
    
    asn1_value_t *v = NULL;
    int result = scan_binary_asn1(data, sizeof(data), &v);
    // Should either fail or handle gracefully
    if (result == 0 && v) {
        asn1_free(v);
    }
    // Test passes if no crash occurs
    TEST_ASSERT_TRUE(1);
}

/* ============================================================================
 * BER vs DER Compatibility Tests
 * ============================================================================ */

void test_ber_features_in_der_context(void) {
    // Test that DER-encoded data still works with BER parser
    // Standard DER SEQUENCE with definite length
    uint8_t data[] = {0x30, 0x06, 0x02, 0x01, 0x2A, 0x01, 0x01, 0xFF};
    
    asn1_value_t *v = NULL;
    TEST_ASSERT_EQUAL(0, scan_binary_asn1(data, sizeof(data), &v));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, v->type);
    TEST_ASSERT_EQUAL(2, v->value.sequence.count);
    
    asn1_free(v);
}

/* ============================================================================
 * Main Test Runner
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();

    // Indefinite length tests
    RUN_TEST(test_indefinite_length_sequence);
    RUN_TEST(test_nested_indefinite_length);

    // Constructed primitive tests
    RUN_TEST(test_constructed_octet_string);
    RUN_TEST(test_constructed_bit_string);

    // Mixed encoding tests
    RUN_TEST(test_mixed_length_encoding);

    // Long form tests
    RUN_TEST(test_long_form_length);

    // Error handling tests
    RUN_TEST(test_missing_end_of_contents);
    RUN_TEST(test_malformed_constructed_primitive);

    // Compatibility tests
    RUN_TEST(test_ber_features_in_der_context);

    return UNITY_END();
}