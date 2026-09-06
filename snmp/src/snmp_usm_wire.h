#ifndef SALTSNET_SNMP_USM_WIRE_H
#define SALTSNET_SNMP_USM_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "snmp_types.h"

int snmp_usm_find_auth_field(const uint8_t *message, size_t message_len,
                             const uint8_t **field, size_t *field_len);

int snmp_usm_encode_security_params_sized(const snmp_usm_params_t *params,
                                          size_t auth_params_len,
                                          size_t priv_params_len,
                                          uint8_t *out, size_t *out_len);

#endif
