/**
 * @file test_asn1_roundtrip.c
 * @brief Test basic ASN.1 encode/decode roundtrip
 */

#include "tinytest.h"
#include "asn1_types.h"
#include <stdio.h>

spec("asn1_roundtrip") {
  describe("ASN.1 Encode/Decode Roundtrip") {
    it("should successfully roundtrip a simple SEQUENCE with INTEGER and OCTET STRING") {
        // Create a simple SEQUENCE with INTEGER and OCTET STRING
        asn1_value_t *seq = asn1_create_sequence();
        asn1_value_t *int_val = asn1_create_integer(42);
        asn1_value_t *str_val = asn1_create_octet_string((uint8_t*)"test", 4);
        
        asn1_sequence_add_child(seq, int_val);
        asn1_sequence_add_child(seq, str_val);
        
        // Encode it
        uint8_t buffer[256];
        size_t buffer_len = sizeof(buffer);
        
        int encode_result = asn1_der_encode(seq, buffer, &buffer_len);
        check_int_eq(encode_result, 0);
        check(buffer_len > 0);
        
        if (encode_result == 0) {
            // Decode it back
            asn1_value_t *decoded = NULL;
            int decode_result = scan_binary_asn1(buffer, buffer_len, &decoded);
            check_int_eq(decode_result, 0);
            check_not_null(decoded);
            
            if (decode_result == 0 && decoded) {
                check_int_eq(decoded->type, ASN1_TYPE_SEQUENCE);
                check_size_eq(decoded->value.sequence.count, 2);
                
                if (decoded->value.sequence.count >= 2) {
                    check_int_eq(decoded->value.sequence.children[0]->type, ASN1_TYPE_INTEGER);
                    check_int_eq((int)decoded->value.sequence.children[0]->value.integer, 42);
                    
                    check_int_eq(decoded->value.sequence.children[1]->type, ASN1_TYPE_OCTET_STRING);
                    check_size_eq(decoded->value.sequence.children[1]->value.octet_string.length, 4);
                    check_mem_eq(decoded->value.sequence.children[1]->value.octet_string.data, "test", 4);
                }
                
                asn1_free(decoded);
            }
        }
        
        asn1_free(seq);
    }
  }
}