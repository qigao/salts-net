#ifndef SALTSNET_SNMP_BUILDER_INTERNAL_H
#define SALTSNET_SNMP_BUILDER_INTERNAL_H

#include "snmp_builder.h"

SALTSNET_SNMP_C_API int snmp_build_v3_query(
    snmp_pdu_type_t pdu_type, int32_t request_id, const snmp_oid_t *oids,
    size_t oid_count, const snmp_usm_params_t *usm_params,
    const snmp_v3_user_t *user, snmp_security_level_t security_level,
    uint8_t *out, size_t *out_len);

#endif
