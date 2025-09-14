/**
 * @file test_asn1_roundtrip.c
 * @brief Test basic ASN.1 encode/decode roundtrip
 */

#include "unity.h"
#include "asn1_types.h"
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

void test_simple_sequence_roundtrip(void) {
    printf("\n=== ASN.1 Roundtrip Test ===\n");
    
    // Create a simple SEQUENCE with INTEGER and OCTET STRING
    asn1_value_t *seq = asn1_create_sequence();
    asn1_value_t *int_val = asn1_create_integer(42);
    asn1_value_t *str_val = asn1_create_octet_string((uint8_t*)"test", 4);
    
    asn1_sequence_add_child(seq, int_val);
    asn1_sequence_add_child(seq, str_val);
    
    printf("Created SEQUENCE with INTEGER(42) and OCTET STRING(\"test\")\n");
    
    // Encode it
    uint8_t buffer[256];
    size_t buffer_len = sizeof(buffer);
    
    printf("Encoding...\n");
    int encode_result = asn1_der_encode(seq, buffer, &buffer_len);
    printf("Encode result: %d (expected 0)\n", encode_result);
    printf("Encoded length: %zu\n", buffer_len);
    
    if (encode_result == 0) {
        printf("Encoded data: ");
        for (size_t i = 0; i < buffer_len && i < 32; i++) {
            printf("%02X ", buffer[i]);
        }
        printf("\n");
        
        // Decode it back
        printf("Decoding...\n");
        asn1_value_t *decoded = NULL;
        int decode_result = scan_binary_asn1(buffer, buffer_len, &decoded);
        printf("Decode result: %d (expected 0)\n", decode_result);
        
        if (decode_result == 0 && decoded) {
            printf("SUCCESS: Roundtrip worked!\n");
            printf("Decoded type: %d (expected %d)\n", decoded->type, ASN1_TYPE_SEQUENCE);
            printf("Decoded children: %zu (expected 2)\n", decoded->value.sequence.count);
            
            if (decoded->value.sequence.count >= 2) {
                printf("Child 0 type: %d, value: %lld\n", 
                       decoded->value.sequence.children[0]->type,
                       (long long)decoded->value.sequence.children[0]->value.integer);
                printf("Child 1 type: %d, length: %zu\n",
                       decoded->value.sequence.children[1]->type,
                       decoded->value.sequence.children[1]->value.octet_string.length);
            }
            
            asn1_free(decoded);
        } else {
            printf("FAILED: Decode error\n");
        }
    } else {
        printf("FAILED: Encode error\n");
    }
    
    asn1_free(seq);
    
    TEST_ASSERT_EQUAL(0, encode_result);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_simple_sequence_roundtrip);
    return UNITY_END();
}