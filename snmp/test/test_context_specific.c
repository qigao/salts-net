/**
 * @file test_context_specific.c
 * @brief Test ASN.1 context-specific tag parsing
 */

#include "tinytest.h"
#include "asn1_types.h"
#include <stdio.h>

spec("context_specific") {
  describe("ASN.1 Context-Specific Tag Parsing") {
    it("should successfully parse a context-specific tag containing an integer") {
        // Test data: A0 03 02 01 2A (Context[0] containing INTEGER 42)
        uint8_t data[] = {0xA0, 0x03, 0x02, 0x01, 0x2A};
        
        asn1_value_t *result = NULL;
        int parse_result = scan_binary_asn1(data, sizeof(data), &result);
        
        check_int_eq(parse_result, 0);
        check_not_null(result);
        
        if (result) {
            check_int_eq(result->tag, 0xA0);
            check_int_eq(result->tag_class, 2); /* context-specific */
            check_int_eq(result->constructed, 1);
            check_int_eq(result->tag_number, 0);
            
            check_int_eq(result->type, TK_CONTEXT_SPECIFIC);
            
            if (result->constructed && result->value.sequence.count > 0) {
                asn1_value_t *child = result->value.sequence.children[0];
                check_int_eq(child->type, ASN1_TYPE_INTEGER);
                if (child->type == ASN1_TYPE_INTEGER) {
                    check_int_eq((int)child->value.integer, 42);
                }
            }
            
            asn1_free(result);
        }
    }

    it("should successfully parse an SNMP-like PDU structure") {
        // Simplified SNMP GetRequest PDU: A0 06 02 01 01 02 01 00
        // Context[0] { INTEGER 1, INTEGER 0 }
        uint8_t data[] = {0xA0, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x00};
        
        asn1_value_t *result = NULL;
        int parse_result = scan_binary_asn1(data, sizeof(data), &result);
        
        check_int_eq(parse_result, 0);
        check_not_null(result);
        
        if (result) {
            check_int_eq(result->tag, 0xA0);
            check_int_eq(result->tag_class, 2);
            check_size_eq(result->value.sequence.count, 2);
            
            asn1_free(result);
        }
    }
  }
}