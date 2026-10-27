#include "tinytest.h"
#include <salts/asn1_value.h>
#include <string.h>

spec("asn1_roundtrip") {
  it("roundtrips protocol values through Salts ASN.1") {
    salts_asn1_value *sequence = NULL, *integer = NULL, *string = NULL;
    salts_asn1_value *decoded = NULL;
    const uint8_t *content = NULL;
    size_t content_size = 0u, written = 0u;
    int64_t number = 0;
    uint8_t buffer[256];
    check_equal(salts_asn1_value_create_sequence(&sequence), SALTS_ASN1_OK);
    check_equal(salts_asn1_value_create_integer(42, &integer), SALTS_ASN1_OK);
    check_equal(salts_asn1_value_create_octet_string("test", 4u, &string),
                SALTS_ASN1_OK);
    check_equal(salts_asn1_value_add_child(sequence, integer), SALTS_ASN1_OK);
    check_equal(salts_asn1_value_add_child(sequence, string), SALTS_ASN1_OK);
    check_equal(salts_asn1_value_encode(sequence, SALTS_ASN1_DER, 0u, buffer,
                                        sizeof(buffer), &written),
                SALTS_ASN1_OK);
    check_equal(salts_asn1_value_decode(buffer, written, SALTS_ASN1_DER,
                                        &decoded),
                SALTS_ASN1_OK);
    check_equal(salts_asn1_value_child_count(decoded), (size_t)2);
    check_equal(salts_asn1_value_get_integer_i64(
                    salts_asn1_value_child_at(decoded, 0u), &number),
                SALTS_ASN1_OK);
    check_equal(number, (int64_t)42);
    check_equal(salts_asn1_value_get_content(
                    salts_asn1_value_child_at(decoded, 1u), &content,
                    &content_size),
                SALTS_ASN1_OK);
    check_equal(content_size, (size_t)4);
    check_equal(content, "test", 4);
    salts_asn1_value_free(decoded);
    salts_asn1_value_free(sequence);
  }
}
