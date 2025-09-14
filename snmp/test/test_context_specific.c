/**
 * @file test_context_specific.c
 * @brief Test ASN.1 context-specific tag parsing
 */

#include "unity.h"
#include "asn1_types.h"
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

void test_context_specific_parsing(void) {
    printf("\n=== Context-Specific Tag Test ===\n");
    
    // Test data: A0 03 02 01 2A (Context[0] containing INTEGER 42)
    uint8_t data[] = {0xA0, 0x03, 0x02, 0x01, 0x2A};
    
    printf("Testing context-specific tag: A0 03 02 01 2A\n");
    printf("Expected: Context[0] containing INTEGER 42\n");
    
    asn1_value_t *result = NULL;
    int parse_result = scan_binary_asn1(data, sizeof(data), &result);
    
    printf("Parse result: %d (expected 0)\n", parse_result);
    
    if (parse_result == 0 && result) {
        printf("SUCCESS: Parsed context-specific tag\n");
        printf("Type: %d\n", result->type);
        printf("Tag: 0x%02X (expected 0xA0)\n", result->tag);
        printf("Tag class: %d (expected 2 for context-specific)\n", result->tag_class);
        printf("Constructed: %d (expected 1)\n", result->constructed);
        printf("Tag number: %d (expected 0)\n", result->tag_number);
        
        if (result->type == TK_CONTEXT_SPECIFIC) {
            printf("Correctly identified as context-specific\n");
            if (result->constructed && result->value.sequence.count > 0) {
                asn1_value_t *child = result->value.sequence.children[0];
                printf("Child type: %d (expected %d for INTEGER)\n", child->type, ASN1_TYPE_INTEGER);
                if (child->type == ASN1_TYPE_INTEGER) {
                    printf("Child value: %lld (expected 42)\n", (long long)child->value.integer);
                }
            }
        }
        
        asn1_free(result);
    } else {
        printf("FAILED: Could not parse context-specific tag\n");
    }
    
    TEST_ASSERT_EQUAL(0, parse_result);
}

void test_snmp_pdu_structure(void) {
    printf("\n=== SNMP PDU Structure Test ===\n");
    
    // Simplified SNMP GetRequest PDU: A0 06 02 01 01 02 01 00
    // Context[0] { INTEGER 1, INTEGER 0 }
    uint8_t data[] = {0xA0, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x00};
    
    printf("Testing SNMP-like PDU: A0 06 02 01 01 02 01 00\n");
    
    asn1_value_t *result = NULL;
    int parse_result = scan_binary_asn1(data, sizeof(data), &result);
    
    printf("Parse result: %d (expected 0)\n", parse_result);
    
    if (parse_result == 0 && result) {
        printf("SUCCESS: Parsed SNMP-like PDU\n");
        printf("Tag: 0x%02X (expected 0xA0)\n", result->tag);
        printf("Tag class: %d (expected 2)\n", result->tag_class);
        printf("Children count: %zu (expected 2)\n", result->value.sequence.count);
        
        asn1_free(result);
    } else {
        printf("FAILED: Could not parse SNMP-like PDU\n");
    }
    
    TEST_ASSERT_EQUAL(0, parse_result);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_context_specific_parsing);
    RUN_TEST(test_snmp_pdu_structure);
    return UNITY_END();
}