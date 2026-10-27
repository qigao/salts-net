#include "tinytest.h"
#include <salts/asn1_value.h>

spec("context_specific") {
  it("parses a constructed context-specific value") {
    const uint8_t data[] = {0xa0, 0x03, 0x02, 0x01, 0x2a};
    salts_asn1_value *value = NULL;
    uint8_t tag_class = 0u, constructed = 0u;
    uint32_t tag_number = 0u;
    int64_t integer = 0;
    check_equal(salts_asn1_value_decode(data, sizeof(data), SALTS_ASN1_BER,
                                        &value),
                SALTS_ASN1_OK);
    check_equal(salts_asn1_value_get_tag(value, &tag_class, &constructed,
                                         &tag_number),
                SALTS_ASN1_OK);
    check_equal(tag_class, 2);
    check_equal(constructed, 1);
    check_equal(tag_number, 0);
    check_equal(salts_asn1_value_child_count(value), (size_t)1);
    check_equal(salts_asn1_value_get_integer_i64(
                    salts_asn1_value_child_at(value, 0u), &integer),
                SALTS_ASN1_OK);
    check_equal(integer, (int64_t)42);
    salts_asn1_value_free(value);
  }

  it("parses an SNMP-like context-specific PDU") {
    const uint8_t data[] = {0xa0, 0x06, 0x02, 0x01,
                            0x01, 0x02, 0x01, 0x00};
    salts_asn1_value *value = NULL;
    check_equal(salts_asn1_value_decode(data, sizeof(data), SALTS_ASN1_BER,
                                        &value),
                SALTS_ASN1_OK);
    check_equal(salts_asn1_value_child_count(value), (size_t)2);
    salts_asn1_value_free(value);
  }
}
