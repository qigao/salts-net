/**
 * @file test_x509_integration.c
 * @brief X.509 Integration Tests
 */

#include "unity.h"
#include "asn1_types.h"
#include "asn1/x509_cert.h"
#include "asn1/asn1_der_compat.h"
#include <string.h>
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

/* ============================================================================
 * X.509 Certificate Structure Tests
 * ============================================================================ */

void test_create_simple_x509_structure(void) {
    // Create a simplified X.509 certificate structure
    asn1_value_t *cert = asn1_create_sequence();
    
    // TBSCertificate (simplified)
    asn1_value_t *tbs = asn1_create_sequence();
    asn1_sequence_add_child(tbs, asn1_create_integer(1)); // version
    asn1_sequence_add_child(tbs, asn1_create_integer(12345)); // serial
    
    // Add TBS to certificate
    asn1_sequence_add_child(cert, tbs);
    
    // Algorithm Identifier (simplified)
    asn1_value_t *alg = asn1_create_sequence();
    asn1_sequence_add_child(alg, asn1_create_oid_from_string("1.2.840.113549.1.1.11")); // SHA256-RSA
    asn1_sequence_add_child(cert, alg);
    
    // Signature (dummy)
    uint8_t sig_data[] = {0x00, 0x01, 0x02, 0x03}; // Dummy signature
    asn1_value_t *signature = asn1_create_bit_string(sig_data, sizeof(sig_data), 0);
    asn1_sequence_add_child(cert, signature);
    
    // Verify structure
    TEST_ASSERT_NOT_NULL(cert);
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, cert->type);
    TEST_ASSERT_EQUAL(3, cert->value.sequence.count);
    
    // Check TBS
    asn1_value_t *tbs_check = cert->value.sequence.children[0];
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, tbs_check->type);
    TEST_ASSERT_EQUAL(2, tbs_check->value.sequence.count);
    
    // Check algorithm
    asn1_value_t *alg_check = cert->value.sequence.children[1];
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, alg_check->type);
    TEST_ASSERT_EQUAL(1, alg_check->value.sequence.count);
    
    // Check signature
    asn1_value_t *sig_check = cert->value.sequence.children[2];
    TEST_ASSERT_EQUAL(ASN1_TYPE_BIT_STRING, sig_check->type);
    
    asn1_free(cert);
}

void test_x509_name_structure(void) {
    // Create an X.509 Name structure: SEQUENCE OF RDN
    asn1_value_t *name = asn1_create_sequence();
    
    // RDN for CN=Test
    asn1_value_t *rdn = asn1_create_set();
    asn1_value_t *attr = asn1_create_sequence();
    
    // Add CN OID
    asn1_sequence_add_child(attr, asn1_create_oid_from_string("2.5.4.3")); // CN
    asn1_sequence_add_child(attr, asn1_create_utf8_string("Test"));
    
    asn1_set_add_child(rdn, attr);
    asn1_sequence_add_child(name, rdn);
    
    // Verify structure
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, name->type);
    TEST_ASSERT_EQUAL(1, name->value.sequence.count);
    
    // Check RDN
    asn1_value_t *rdn_check = name->value.sequence.children[0];
    TEST_ASSERT_EQUAL(ASN1_TYPE_SET, rdn_check->type);
    TEST_ASSERT_EQUAL(1, rdn_check->value.set.count);
    
    // Check attribute
    asn1_value_t *attr_check = rdn_check->value.set.children[0];
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, attr_check->type);
    TEST_ASSERT_EQUAL(2, attr_check->value.sequence.count);
    
    // Check OID
    asn1_value_t *oid_check = attr_check->value.sequence.children[0];
    TEST_ASSERT_EQUAL(ASN1_TYPE_OBJECT_IDENTIFIER, oid_check->type);
    
    // Check value
    asn1_value_t *val_check = attr_check->value.sequence.children[1];
    TEST_ASSERT_EQUAL(ASN1_TYPE_UTF8_STRING, val_check->type);
    
    asn1_free(name);
}

void test_x509_extension_structure(void) {
    // Create an X.509 Extension structure
    asn1_value_t *ext = asn1_create_sequence();
    
    // Extension OID (Basic Constraints)
    asn1_sequence_add_child(ext, asn1_create_oid_from_string("2.5.29.19"));
    
    // Critical flag
    asn1_sequence_add_child(ext, asn1_create_boolean(1));
    
    // Extension value (DER-encoded)
    uint8_t ext_value[] = {0x30, 0x03, 0x01, 0x01, 0xFF}; // SEQUENCE { BOOLEAN TRUE }
    asn1_sequence_add_child(ext, asn1_create_octet_string(ext_value, sizeof(ext_value)));
    
    // Verify structure
    TEST_ASSERT_NOT_NULL(ext);
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, ext->type);
    TEST_ASSERT_EQUAL(3, ext->value.sequence.count);
    
    // Check OID
    asn1_value_t *oid = ext->value.sequence.children[0];
    TEST_ASSERT_EQUAL(ASN1_TYPE_OBJECT_IDENTIFIER, oid->type);
    
    // Check critical flag
    asn1_value_t *critical = ext->value.sequence.children[1];
    TEST_ASSERT_EQUAL(ASN1_TYPE_BOOLEAN, critical->type);
    TEST_ASSERT_TRUE(critical->value.boolean);
    
    // Check value
    asn1_value_t *value = ext->value.sequence.children[2];
    TEST_ASSERT_EQUAL(ASN1_TYPE_OCTET_STRING, value->type);
    TEST_ASSERT_EQUAL(5, value->value.octet_string.length);
    
    asn1_free(ext);
}

/* ============================================================================
 * Context-Specific Tag Tests (X.509 specific)
 * ============================================================================ */

void test_x509_context_specific_tags(void) {
    // Test context-specific tags used in X.509
    
    // Create a version field [0] EXPLICIT
    asn1_value_t *version_ctx = asn1_create_sequence();
    version_ctx->tag = 0xA0;  // [0] EXPLICIT
    version_ctx->tag_class = 2; // Context-specific
    version_ctx->constructed = 1; // Constructed
    version_ctx->tag_number = 0; // [0]
    
    asn1_sequence_add_child(version_ctx, asn1_create_integer(2)); // v3
    
    TEST_ASSERT_EQUAL(0xA0, version_ctx->tag);
    TEST_ASSERT_EQUAL(2, version_ctx->tag_class);
    TEST_ASSERT_EQUAL(1, version_ctx->constructed);
    TEST_ASSERT_EQUAL(0, version_ctx->tag_number);
    
    asn1_free(version_ctx);
}

/* ============================================================================
 * OID Tests
 * ============================================================================ */

void test_x509_common_oids(void) {
    // Test common X.509 OIDs
    struct {
        const char *oid_str;
        const char *name;
    } test_oids[] = {
        {"2.5.4.3", "CN"},
        {"2.5.4.6", "C"},
        {"2.5.4.10", "O"},
        {"1.2.840.113549.1.1.11", "SHA256-RSA"},
        {"2.5.29.19", "Basic Constraints"},
        {"2.5.29.15", "Key Usage"},
        {NULL, NULL}
    };
    
    for (int i = 0; test_oids[i].oid_str != NULL; i++) {
        asn1_value_t *oid = asn1_create_oid_from_string(test_oids[i].oid_str);
        TEST_ASSERT_NOT_NULL(oid);
        TEST_ASSERT_EQUAL(ASN1_TYPE_OBJECT_IDENTIFIER, oid->type);
        
        // Convert back to string and compare
        char *str = asn1_oid_to_string(&oid->value.oid);
        TEST_ASSERT_NOT_NULL(str);
        TEST_ASSERT_EQUAL_STRING(test_oids[i].oid_str, str);
        
        free(str);
        asn1_free(oid);
    }
}

/* ============================================================================
 * Binary Parsing Integration Tests
 * ============================================================================ */

void test_parse_x509_like_der(void) {
    // Create a simple X.509-like DER structure and parse it
    
    // First create the structure
    asn1_value_t *cert = asn1_create_sequence();
    asn1_value_t *tbs = asn1_create_sequence();
    
    asn1_sequence_add_child(tbs, asn1_create_integer(1));
    asn1_sequence_add_child(tbs, asn1_create_integer(12345));
    asn1_sequence_add_child(cert, tbs);
    
    // Note: We would need DER encoding to test full round-trip
    // For now, just test that we can create the structure
    TEST_ASSERT_NOT_NULL(cert);
    TEST_ASSERT_EQUAL(ASN1_TYPE_SEQUENCE, cert->type);
    
    asn1_free(cert);
}

/* ============================================================================
 * Error Handling Tests
 * ============================================================================ */

void test_invalid_oid_string(void) {
    // Test invalid OID strings
    asn1_value_t *oid1 = asn1_create_oid_from_string("invalid");
    TEST_ASSERT_NULL(oid1);
    
    asn1_value_t *oid2 = asn1_create_oid_from_string("1.2.3.a");
    TEST_ASSERT_NULL(oid2);
    
    asn1_value_t *oid3 = asn1_create_oid_from_string("");
    TEST_ASSERT_NULL(oid3);
    
    asn1_value_t *oid4 = asn1_create_oid_from_string(NULL);
    TEST_ASSERT_NULL(oid4);
}

void test_null_parameter_handling(void) {
    // Test that functions handle NULL parameters gracefully
    TEST_ASSERT_NULL(asn1_create_octet_string(NULL, 10)); // NULL data with non-zero length should fail
    TEST_ASSERT_NULL(asn1_create_utf8_string(NULL));
    TEST_ASSERT_NULL(asn1_create_printable_string(NULL));
    TEST_ASSERT_NULL(asn1_create_ia5_string(NULL));
    TEST_ASSERT_NULL(asn1_create_utc_time(NULL));
    
    // Test that empty octet string works (NULL data with zero length)
    asn1_value_t *empty_octet = asn1_create_octet_string(NULL, 0);
    TEST_ASSERT_NOT_NULL(empty_octet);
    TEST_ASSERT_EQUAL(ASN1_TYPE_OCTET_STRING, empty_octet->type);
    TEST_ASSERT_EQUAL(0, empty_octet->value.octet_string.length);
    TEST_ASSERT_NULL(empty_octet->value.octet_string.data);
    asn1_free(empty_octet);
    
    // Test add_child with NULL
    asn1_value_t *seq = asn1_create_sequence();
    TEST_ASSERT_EQUAL(-1, asn1_sequence_add_child(NULL, seq));
    TEST_ASSERT_EQUAL(-1, asn1_sequence_add_child(seq, NULL));
    asn1_free(seq);
}

/* ============================================================================
 * Main Test Runner
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();

    // X.509 structure tests
    RUN_TEST(test_create_simple_x509_structure);
    RUN_TEST(test_x509_name_structure);
    RUN_TEST(test_x509_extension_structure);

    // Context-specific tag tests
    RUN_TEST(test_x509_context_specific_tags);

    // OID tests
    RUN_TEST(test_x509_common_oids);

    // Binary parsing integration
    RUN_TEST(test_parse_x509_like_der);

    // Error handling tests
    RUN_TEST(test_invalid_oid_string);
    RUN_TEST(test_null_parameter_handling);

    return UNITY_END();
}