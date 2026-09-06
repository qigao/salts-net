#include "snmp_usm_wire.h"

#include <limits.h>

typedef struct snmp_ber_tlv {
    uint8_t tag;
    const uint8_t *value;
    size_t value_len;
    size_t total_len;
} snmp_ber_tlv_t;

static int snmp_ber_read_tlv(const uint8_t *data, size_t size,
                             snmp_ber_tlv_t *out) {
    size_t header_len = 2u;
    size_t value_len = 0u;
    if (!data || !out || size < 2u || (data[0] & 0x1fu) == 0x1fu) return -1;

    if (data[1] < 0x80u) {
        value_len = data[1];
    } else {
        const size_t octets = data[1] & 0x7fu;
        if (octets == 0u || octets > sizeof(size_t) || octets > size - 2u) return -1;
        header_len += octets;
        for (size_t index = 0; index < octets; ++index) {
            if (value_len > (SIZE_MAX >> 8u)) return -1;
            value_len = (value_len << 8u) | data[2u + index];
        }
    }
    if (header_len > size || value_len > size - header_len) return -1;

    out->tag = data[0];
    out->value = data + header_len;
    out->value_len = value_len;
    out->total_len = header_len + value_len;
    return 0;
}

static int snmp_ber_child(const snmp_ber_tlv_t *parent, size_t child_index,
                          snmp_ber_tlv_t *out) {
    size_t offset = 0u;
    if (!parent || !out) return -1;
    for (size_t index = 0; index <= child_index; ++index) {
        snmp_ber_tlv_t child;
        if (offset >= parent->value_len ||
            snmp_ber_read_tlv(parent->value + offset, parent->value_len - offset,
                              &child) != 0) {
            return -1;
        }
        if (index == child_index) {
            *out = child;
            return 0;
        }
        offset += child.total_len;
    }
    return -1;
}

int snmp_usm_find_auth_field(const uint8_t *message, size_t message_len,
                             const uint8_t **field, size_t *field_len) {
    snmp_ber_tlv_t message_sequence;
    snmp_ber_tlv_t security_parameters;
    snmp_ber_tlv_t usm_sequence;
    snmp_ber_tlv_t auth_parameters;
    if (!message || !field || !field_len ||
        snmp_ber_read_tlv(message, message_len, &message_sequence) != 0 ||
        message_sequence.tag != 0x30u || message_sequence.total_len != message_len ||
        snmp_ber_child(&message_sequence, 2u, &security_parameters) != 0 ||
        security_parameters.tag != 0x04u ||
        snmp_ber_read_tlv(security_parameters.value, security_parameters.value_len,
                          &usm_sequence) != 0 ||
        usm_sequence.tag != 0x30u ||
        usm_sequence.total_len != security_parameters.value_len ||
        snmp_ber_child(&usm_sequence, 4u, &auth_parameters) != 0 ||
        auth_parameters.tag != 0x04u) {
        return -1;
    }
    *field = auth_parameters.value;
    *field_len = auth_parameters.value_len;
    return 0;
}
