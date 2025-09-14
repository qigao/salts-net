/**
 * @file snmp_builder.c
 * @brief SNMP Message Builder Implementation
 */

#include "snmp_builder.h"
#include "snmp_usm.h"
#include "asn1_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stb_sprintf.h>

/* Helper: Build PDU with NULL values for Get/GetNext */
static int build_get_pdu(
    snmp_pdu_type_t pdu_type,
    int32_t request_id,
    const snmp_oid_t *oids,
    size_t oid_count,
    asn1_value_t **out_pdu
) {
    /* Create PDU SEQUENCE */
    asn1_value_t *pdu = asn1_create_sequence();
    if (!pdu) return SNMP_BUILD_ERROR_MEMORY;

    /* Override tag to context-specific */
    pdu->tag = pdu_type;
    pdu->tag_class = 2;  // Context-specific
    pdu->tag_number = pdu_type & 0x1F;  // Extract tag number from pdu_type

    /* Add request-id */
    asn1_value_t *req_id = asn1_create_integer(request_id);
    if (!req_id || asn1_sequence_add_child(pdu, req_id) < 0) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }

    /* Add error-status (0) */
    asn1_value_t *error_status = asn1_create_integer(0);
    if (!error_status || asn1_sequence_add_child(pdu, error_status) < 0) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }

    /* Add error-index (0) */
    asn1_value_t *error_index = asn1_create_integer(0);
    if (!error_index || asn1_sequence_add_child(pdu, error_index) < 0) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }

    /* Add variable-bindings SEQUENCE */
    asn1_value_t *varbinds = asn1_create_sequence();
    if (!varbinds) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }

    for (size_t i = 0; i < oid_count; i++) {
        /* Create VarBind SEQUENCE { oid, NULL } */
        asn1_value_t *varbind = asn1_create_sequence();
        if (!varbind) {
            asn1_free(varbinds);
            asn1_free(pdu);
            return SNMP_BUILD_ERROR_MEMORY;
        }

        /* Add OID */
        asn1_value_t *oid_val = asn1_create_oid(oids[i].components, oids[i].count);
        if (!oid_val || asn1_sequence_add_child(varbind, oid_val) < 0) {
            asn1_free(varbind);
            asn1_free(varbinds);
            asn1_free(pdu);
            return SNMP_BUILD_ERROR_MEMORY;
        }

        /* Add NULL value */
        asn1_value_t *null_val = asn1_create_null();
        if (!null_val || asn1_sequence_add_child(varbind, null_val) < 0) {
            asn1_free(varbind);
            asn1_free(varbinds);
            asn1_free(pdu);
            return SNMP_BUILD_ERROR_MEMORY;
        }

        if (asn1_sequence_add_child(varbinds, varbind) < 0) {
            asn1_free(varbinds);
            asn1_free(pdu);
            return SNMP_BUILD_ERROR_MEMORY;
        }
    }

    if (asn1_sequence_add_child(pdu, varbinds) < 0) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }

    *out_pdu = pdu;
    return SNMP_BUILD_OK;
}

/* Common message builder */
static int build_message(
    snmp_version_t version,
    const char *community,
    asn1_value_t *pdu,
    uint8_t *out,
    size_t *out_len
) {
    /* Create message SEQUENCE */
    asn1_value_t *msg = asn1_create_sequence();
    if (!msg) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }

    /* Add version */
    asn1_value_t *ver = asn1_create_integer(version);
    if (!ver || asn1_sequence_add_child(msg, ver) < 0) {
        asn1_free(msg);
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }

    /* Add community */
    asn1_value_t *comm = asn1_create_octet_string(
        (const uint8_t *)community,
        strlen(community)
    );
    if (!comm || asn1_sequence_add_child(msg, comm) < 0) {
        asn1_free(msg);
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }

    /* Add PDU */
    if (asn1_sequence_add_child(msg, pdu) < 0) {
        asn1_free(msg);
        return SNMP_BUILD_ERROR_MEMORY;
    }

    /* Encode to DER */
    int result = asn1_der_encode(msg, out, out_len);
    asn1_free(msg);

    if (result < 0) {
        return SNMP_BUILD_ERROR_BUFFER;
    }

    return SNMP_BUILD_OK;
}

int snmp_build_get_request(
    snmp_version_t version,
    const char *community,
    int32_t request_id,
    const snmp_oid_t *oids,
    size_t oid_count,
    uint8_t *out,
    size_t *out_len
) {
    if (!community || !oids || oid_count == 0 || !out || !out_len) {
        return SNMP_BUILD_ERROR_INVALID;
    }

    asn1_value_t *pdu = NULL;
    int result = build_get_pdu(SNMP_PDU_GET_REQUEST, request_id, oids, oid_count, &pdu);
    if (result != SNMP_BUILD_OK) {
        return result;
    }

    return build_message(version, community, pdu, out, out_len);
}

int snmp_build_get_next_request(
    snmp_version_t version,
    const char *community,
    int32_t request_id,
    const snmp_oid_t *oids,
    size_t oid_count,
    uint8_t *out,
    size_t *out_len
) {
    if (!community || !oids || oid_count == 0 || !out || !out_len) {
        return SNMP_BUILD_ERROR_INVALID;
    }

    asn1_value_t *pdu = NULL;
    int result = build_get_pdu(SNMP_PDU_GET_NEXT_REQUEST, request_id, oids, oid_count, &pdu);
    if (result != SNMP_BUILD_OK) {
        return result;
    }

    return build_message(version, community, pdu, out, out_len);
}

int snmp_build_set_request(
    snmp_version_t version,
    const char *community,
    int32_t request_id,
    const snmp_varbind_t *varbinds,
    size_t varbind_count,
    uint8_t *out,
    size_t *out_len
) {
    /* TODO: Implement SetRequest with typed values */
    (void)version;
    (void)community;
    (void)request_id;
    (void)varbinds;
    (void)varbind_count;
    (void)out;
    (void)out_len;
    return SNMP_BUILD_ERROR_INVALID;  /* Not implemented yet */
}

int snmp_build_get_bulk_request(
    const char *community,
    int32_t request_id,
    int32_t non_repeaters,
    int32_t max_repetitions,
    const snmp_oid_t *oids,
    size_t oid_count,
    uint8_t *out,
    size_t *out_len
) {
    /* TODO: Implement GetBulkRequest */
    (void)community;
    (void)request_id;
    (void)non_repeaters;
    (void)max_repetitions;
    (void)oids;
    (void)oid_count;
    (void)out;
    (void)out_len;
    return SNMP_BUILD_ERROR_INVALID;  /* Not implemented yet */
}

/* ============================================================================
 * OID Helper Functions
 * ============================================================================ */

int snmp_oid_from_string(const char *oid_str, snmp_oid_t *oid) {
    if (!oid_str || !oid) {
        return -1;
    }

    /* Count components */
    size_t count = 1;
    for (const char *p = oid_str; *p; p++) {
        if (*p == '.') count++;
    }

    /* Allocate components array */
    oid->components = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!oid->components) {
        return -1;
    }

    /* Parse components */
    oid->count = 0;
    const char *p = oid_str;
    while (*p && oid->count < count) {
        char *end;
        unsigned long val = strtoul(p, &end, 10);
        if (p == end) break;
        oid->components[oid->count++] = (uint32_t)val;
        p = (*end == '.') ? end + 1 : end;
    }

    if (oid->count < 2) {
        free(oid->components);
        return -1;
    }

    return 0;
}

int snmp_oid_to_string(const snmp_oid_t *oid, char *buf, size_t buf_len) {
    if (!oid || !buf || buf_len == 0) {
        return -1;
    }

    size_t offset = 0;
    for (size_t i = 0; i < oid->count; i++) {
        int written = stbsp_snprintf(buf + offset, buf_len - offset,
                              "%s%u", (i > 0) ? "." : "", oid->components[i]);
        if (written < 0 || (size_t)written >= buf_len - offset) {
            return -1;  /* Buffer too small */
        }
        offset += written;
    }

    return 0;
}

int snmp_oid_compare(const snmp_oid_t *oid1, const snmp_oid_t *oid2) {
    if (!oid1 || !oid2) {
        return (oid1 == oid2) ? 0 : (oid1 ? 1 : -1);
    }

    size_t min_count = (oid1->count < oid2->count) ? oid1->count : oid2->count;

    for (size_t i = 0; i < min_count; i++) {
        if (oid1->components[i] != oid2->components[i]) {
            return (oid1->components[i] < oid2->components[i]) ? -1 : 1;
        }
    }

    /* All compared components are equal, check length */
    if (oid1->count != oid2->count) {
        return (oid1->count < oid2->count) ? -1 : 1;
    }

    return 0;  /* Equal */
}

void snmp_oid_free(snmp_oid_t *oid) {
    if (oid && oid->components) {
        free(oid->components);
        oid->components = NULL;
        oid->count = 0;
    }
}

/*
 * Build SNMPv3 GetRequest with USM security
 *
 * RFC 3412 SNMPv3Message structure:
 * SNMPv3Message ::= SEQUENCE {
 *     msgVersion INTEGER (3),
 *     msgGlobalData HeaderData,
 *     msgSecurityParameters OCTET STRING,
 *     msgData ScopedPduData
 * }
 */
int snmp_build_v3_get_request(
    int32_t request_id,
    const snmp_oid_t *oids,
    size_t oid_count,
    const snmp_usm_params_t *usm_params,
    const snmp_v3_user_t *user,
    snmp_security_level_t security_level,
    uint8_t *out,
    size_t *out_len
) {
    if (!oids || oid_count == 0 || !usm_params || !out || !out_len) {
        return SNMP_BUILD_ERROR_INVALID;
    }

    /* Step 1: Build PDU */
    asn1_value_t *pdu = NULL;
    int result = build_get_pdu(SNMP_PDU_GET_REQUEST, request_id, oids, oid_count, &pdu);
    if (result != SNMP_BUILD_OK) {
        return result;
    }

    /* Step 2: Build scopedPDU */
    asn1_value_t *scoped_pdu = asn1_create_sequence();
    if (!scoped_pdu) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }

    /* contextEngineID (same as authoritative) */
    asn1_value_t *context_engine_id = asn1_create_octet_string(
        usm_params->authoritative_engine_id,
        usm_params->engine_id_len
    );
    asn1_sequence_add_child(scoped_pdu, context_engine_id);

    /* contextName (empty string) */
    asn1_value_t *context_name = asn1_create_octet_string(NULL, 0);
    asn1_sequence_add_child(scoped_pdu, context_name);

    /* PDU */
    asn1_sequence_add_child(scoped_pdu, pdu);

    /* Step 3: Encrypt scopedPDU if privacy enabled */
    uint8_t scoped_pdu_data[2048];
    size_t scoped_pdu_len = sizeof(scoped_pdu_data);
    result = asn1_der_encode(scoped_pdu, scoped_pdu_data, &scoped_pdu_len);
    if (result != 0) {
        asn1_free(scoped_pdu);
        return SNMP_BUILD_ERROR_INVALID;
    }

    uint8_t encrypted_pdu[2048];
    size_t encrypted_len = sizeof(encrypted_pdu);
    uint8_t priv_params[8] = {0};

    if (security_level == SNMP_SEC_LEVEL_AUTH_PRIV && user) {
        /* Encrypt */
        result = usm_encrypt(
            scoped_pdu_data,
            scoped_pdu_len,
            user->priv_key,
            user->priv_key_len,
            user->priv_protocol,
            usm_params->engine_boots,
            usm_params->engine_time,
            priv_params,
            encrypted_pdu,
            &encrypted_len
        );

        if (result != USM_OK) {
            asn1_free(scoped_pdu);
            return SNMP_BUILD_ERROR_INVALID;
        }
    }

    asn1_free(scoped_pdu);  /* Don't need this anymore */

    /* Step 4: Encode USM security parameters (with placeholder auth_params) */
    snmp_usm_params_t usm_params_copy = *usm_params;
    memcpy(usm_params_copy.priv_params, priv_params, 8);
    memset(usm_params_copy.auth_params, 0, 12);  /* Placeholder, will compute HMAC later */

    uint8_t security_params[256];
    size_t security_params_len = sizeof(security_params);
    result = usm_encode_security_params(&usm_params_copy, security_params, &security_params_len);
    if (result != USM_OK) {
        return SNMP_BUILD_ERROR_INVALID;
    }

    /* Step 5: Build msgGlobalData */
    asn1_value_t *msg_global_data = asn1_create_sequence();
    asn1_sequence_add_child(msg_global_data, asn1_create_integer(request_id));  /* msgID */
    asn1_sequence_add_child(msg_global_data, asn1_create_integer(65507));       /* msgMaxSize */

    /* msgFlags */
    uint8_t msg_flags = 0x04;  /* reportable */
    if (security_level >= SNMP_SEC_LEVEL_AUTH_NOPRIV) msg_flags |= 0x01;  /* auth */
    if (security_level == SNMP_SEC_LEVEL_AUTH_PRIV) msg_flags |= 0x02;    /* priv */
    asn1_value_t *flags = asn1_create_octet_string(&msg_flags, 1);
    asn1_sequence_add_child(msg_global_data, flags);

    asn1_sequence_add_child(msg_global_data, asn1_create_integer(3));  /* msgSecurityModel = USM */

    /* Step 6: Build msgData (plaintext or encrypted) */
    asn1_value_t *msg_data = NULL;
    if (security_level == SNMP_SEC_LEVEL_AUTH_PRIV) {
        /* Encrypted - wrap in OCTET STRING */
        msg_data = asn1_create_octet_string(encrypted_pdu, encrypted_len);
    } else {
        /* Plaintext - use scopedPDU as SEQUENCE */
        msg_data = asn1_create_sequence();
        if (!msg_data) {
            asn1_free(scoped_pdu);
            return SNMP_BUILD_ERROR_MEMORY;
        }

        asn1_sequence_add_child(msg_data, asn1_create_octet_string(
            usm_params->authoritative_engine_id, usm_params->engine_id_len));
        asn1_sequence_add_child(msg_data, asn1_create_octet_string(NULL, 0));  /* contextName */

        /* Re-build PDU */
        asn1_value_t *pdu2 = NULL;
        result = build_get_pdu(SNMP_PDU_GET_REQUEST, request_id, oids, oid_count, &pdu2);
        if (result != SNMP_BUILD_OK) {
            asn1_free(msg_data);
            asn1_free(scoped_pdu);
            return result;
        }
        asn1_sequence_add_child(msg_data, pdu2);
    }

    /* Step 7: Build complete SNMPv3Message */
    asn1_value_t *msg = asn1_create_sequence();
    asn1_sequence_add_child(msg, asn1_create_integer(3));  /* msgVersion */
    asn1_sequence_add_child(msg, msg_global_data);
    asn1_sequence_add_child(msg, asn1_create_octet_string(security_params, security_params_len));
    asn1_sequence_add_child(msg, msg_data);

    /* Step 8: Encode to output buffer */
    int encode_result = asn1_der_encode(msg, out, out_len);
    if (encode_result != 0) {
        asn1_free(msg);
        return SNMP_BUILD_ERROR_INVALID;
    }

    /* Step 9: Compute HMAC if authentication enabled */
    if (security_level >= SNMP_SEC_LEVEL_AUTH_NOPRIV && user) {
        uint8_t auth_params[12];
        result = usm_compute_auth(out, *out_len, user->auth_key, user->auth_key_len,
                                   user->auth_protocol, auth_params);

        if (result == USM_OK) {
            /* Find msgAuthenticationParameters field in the encoded message */
            /* Pattern: 0x04 0x0C followed by 12 zero bytes */
            uint8_t pattern[14] = {0x04, 0x0C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

            for (size_t i = 0; i + 14 <= *out_len; i++) {
                if (memcmp(out + i, pattern, 14) == 0) {
                    /* Found it - patch the auth_params (skip tag+length, update value) */
                    memcpy(out + i + 2, auth_params, 12);
                    break;
                }
            }
        }
    }

    asn1_free(msg);
    return SNMP_BUILD_OK;
}
