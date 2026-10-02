/**
 * @file snmp_builder.c
 * @brief SNMP Message Builder Implementation
 */

#include "snmp_builder.h"
#include "snmp_builder_internal.h"
#include "snmp_usm_wire.h"
#include "snmp_usm.h"
#include "asn1_types.h"
#include <fmt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

static int snmp_builder_add_child(asn1_value_t *parent, asn1_value_t *child) {
    if (!child) return -1;
    if (asn1_sequence_add_child(parent, child) < 0) {
        asn1_free(child);
        return -1;
    }
    return 0;
}

static asn1_value_t *build_varbind_value(const snmp_varbind_t *varbind) {
    asn1_value_t *value = NULL;
    if (!varbind) return NULL;

    switch (varbind->value_type) {
        case SNMP_TYPE_INTEGER:
            return asn1_create_integer(varbind->value.i32);
        case SNMP_TYPE_OCTET_STRING:
            return asn1_create_octet_string(varbind->value.bytes.data,
                                            varbind->value.bytes.len);
        case SNMP_TYPE_NULL:
            return asn1_create_null();
        case SNMP_TYPE_OID:
            return asn1_create_oid(varbind->value.oid.components,
                                   varbind->value.oid.count);
        case SNMP_TYPE_IPADDRESS:
        case SNMP_TYPE_OPAQUE:
            value = asn1_create_octet_string(varbind->value.bytes.data,
                                             varbind->value.bytes.len);
            break;
        case SNMP_TYPE_COUNTER32:
        case SNMP_TYPE_GAUGE32:
        case SNMP_TYPE_TIMETICKS:
            if (varbind->value.i32 < 0) return NULL;
            value = asn1_create_integer((uint32_t)varbind->value.i32);
            break;
        case SNMP_TYPE_COUNTER64:
            if (varbind->value.i64 < 0) return NULL;
            value = asn1_create_integer(varbind->value.i64);
            break;
        default:
            return NULL;
    }

    if (value) {
        value->tag_class = 1;
        value->tag_number = (uint32_t)varbind->value_type & 0x1fu;
        value->tag = (uint8_t)varbind->value_type;
    }
    return value;
}

static int build_set_pdu(int32_t request_id, const snmp_varbind_t *varbinds,
                         size_t varbind_count, asn1_value_t **out_pdu) {
    asn1_value_t *pdu = NULL;
    asn1_value_t *varbind_list = NULL;
    if (!varbinds || varbind_count == 0u || !out_pdu) {
        return SNMP_BUILD_ERROR_INVALID;
    }

    pdu = asn1_create_sequence();
    if (!pdu) return SNMP_BUILD_ERROR_MEMORY;
    pdu->tag = SNMP_PDU_SET_REQUEST;
    pdu->tag_class = 2;
    pdu->tag_number = SNMP_PDU_SET_REQUEST & 0x1fu;

    if (snmp_builder_add_child(pdu, asn1_create_integer(request_id)) < 0 ||
        snmp_builder_add_child(pdu, asn1_create_integer(0)) < 0 ||
        snmp_builder_add_child(pdu, asn1_create_integer(0)) < 0) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }

    varbind_list = asn1_create_sequence();
    if (!varbind_list) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }
    for (size_t i = 0; i < varbind_count; ++i) {
        asn1_value_t *varbind = NULL;
        asn1_value_t *oid = NULL;
        asn1_value_t *value = NULL;
        if (!varbinds[i].oid.components || varbinds[i].oid.count < 2u) {
            asn1_free(varbind_list);
            asn1_free(pdu);
            return SNMP_BUILD_ERROR_INVALID;
        }
        varbind = asn1_create_sequence();
        oid = asn1_create_oid(varbinds[i].oid.components, varbinds[i].oid.count);
        value = build_varbind_value(&varbinds[i]);
        if (!varbind || !oid || !value) {
            asn1_free(varbind);
            asn1_free(oid);
            asn1_free(value);
            asn1_free(varbind_list);
            asn1_free(pdu);
            return value ? SNMP_BUILD_ERROR_MEMORY : SNMP_BUILD_ERROR_INVALID;
        }
        if (snmp_builder_add_child(varbind, oid) < 0) {
            asn1_free(value);
            asn1_free(varbind);
            asn1_free(varbind_list);
            asn1_free(pdu);
            return SNMP_BUILD_ERROR_MEMORY;
        }
        oid = NULL;
        if (snmp_builder_add_child(varbind, value) < 0) {
            asn1_free(varbind);
            asn1_free(varbind_list);
            asn1_free(pdu);
            return SNMP_BUILD_ERROR_MEMORY;
        }
        value = NULL;
        if (snmp_builder_add_child(varbind_list, varbind) < 0) {
            asn1_free(varbind_list);
            asn1_free(pdu);
            return SNMP_BUILD_ERROR_MEMORY;
        }
        varbind = NULL;
    }
    if (snmp_builder_add_child(pdu, varbind_list) < 0) {
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
    if (version != SNMP_VERSION_1 && version != SNMP_VERSION_2C) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_INVALID;
    }
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
    asn1_value_t *pdu = NULL;
    int result;
    if (!community || !varbinds || varbind_count == 0u || !out || !out_len) {
        return SNMP_BUILD_ERROR_INVALID;
    }
    result = build_set_pdu(request_id, varbinds, varbind_count, &pdu);
    if (result != SNMP_BUILD_OK) return result;
    return build_message(version, community, pdu, out, out_len);
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
    asn1_value_t *pdu = NULL;
    int result;
    if (!community || non_repeaters < 0 || max_repetitions < 0 || !oids ||
        oid_count == 0u || !out || !out_len) {
        return SNMP_BUILD_ERROR_INVALID;
    }
    result = build_get_pdu(SNMP_PDU_GET_BULK_REQUEST, request_id, oids,
                           oid_count, &pdu);
    if (result != SNMP_BUILD_OK) return result;
    pdu->value.sequence.children[1]->value.integer = non_repeaters;
    pdu->value.sequence.children[2]->value.integer = max_repetitions;
    return build_message(SNMP_VERSION_2C, community, pdu, out, out_len);
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
        int written = fmt(buf + offset, buf_len - offset,
                          "{}{}", (i > 0) ? "." : "", oid->components[i]);
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
static int snmp_build_v3_pdu(asn1_value_t *pdu, int32_t request_id,
                             int reportable,
                             const snmp_usm_params_t *usm_params,
                             const snmp_v3_user_t *user,
                             snmp_security_level_t security_level,
                             uint8_t *out, size_t *out_len) {
    asn1_value_t *scoped_pdu = NULL;
    asn1_value_t *msg_data = NULL;
    asn1_value_t *global_data = NULL;
    asn1_value_t *msg = NULL;
    uint8_t scoped_pdu_data[2048];
    size_t scoped_pdu_len = sizeof(scoped_pdu_data);
    uint8_t encrypted_pdu[2048];
    size_t encrypted_len = sizeof(encrypted_pdu);
    uint8_t priv_params[8] = {0};
    uint8_t security_params[256];
    size_t security_params_len = sizeof(security_params);
    snmp_usm_params_t params_copy;
    uint8_t msg_flags;
    int result;

    if (!pdu || !usm_params || !out || !out_len) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_INVALID;
    }
    if (security_level != SNMP_SEC_LEVEL_NOAUTH_NOPRIV &&
        security_level != SNMP_SEC_LEVEL_AUTH_NOPRIV &&
        security_level != SNMP_SEC_LEVEL_AUTH_PRIV) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_INVALID;
    }
    if (security_level >= SNMP_SEC_LEVEL_AUTH_NOPRIV &&
        (!user || !user->user_name || !usm_params->user_name ||
         strcmp(user->user_name, usm_params->user_name) != 0 ||
         (user->auth_protocol != SNMP_AUTH_MD5 &&
          user->auth_protocol != SNMP_AUTH_SHA1) ||
         (user->auth_protocol == SNMP_AUTH_MD5 && user->auth_key_len != 16u) ||
         (user->auth_protocol == SNMP_AUTH_SHA1 && user->auth_key_len != 20u))) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_INVALID;
    }
    if (security_level == SNMP_SEC_LEVEL_AUTH_PRIV &&
        (user->priv_protocol == SNMP_PRIV_NONE ||
         (user->priv_protocol == SNMP_PRIV_DES && user->priv_key_len < 16u) ||
         (user->priv_protocol == SNMP_PRIV_AES128 && user->priv_key_len < 16u))) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_INVALID;
    }

    scoped_pdu = asn1_create_sequence();
    if (!scoped_pdu) {
        asn1_free(pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }
    if (snmp_builder_add_child(
            scoped_pdu,
            asn1_create_octet_string(usm_params->authoritative_engine_id,
                                     usm_params->engine_id_len)) < 0 ||
        snmp_builder_add_child(scoped_pdu,
                               asn1_create_octet_string(NULL, 0)) < 0) {
        asn1_free(pdu);
        asn1_free(scoped_pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }
    if (snmp_builder_add_child(scoped_pdu, pdu) < 0) {
        asn1_free(scoped_pdu);
        return SNMP_BUILD_ERROR_MEMORY;
    }
    pdu = NULL;

    if (security_level == SNMP_SEC_LEVEL_AUTH_PRIV) {
        result = asn1_der_encode(scoped_pdu, scoped_pdu_data, &scoped_pdu_len);
        if (result != 0) {
            asn1_free(scoped_pdu);
            return SNMP_BUILD_ERROR_BUFFER;
        }
        result = usm_encrypt(
            scoped_pdu_data, scoped_pdu_len, user->priv_key, user->priv_key_len,
            user->priv_protocol, usm_params->engine_boots,
            usm_params->engine_time, priv_params, encrypted_pdu, &encrypted_len);
        if (result != USM_OK) {
            asn1_free(scoped_pdu);
            return SNMP_BUILD_ERROR_INVALID;
        }
        asn1_free(scoped_pdu);
        scoped_pdu = NULL;
        msg_data = asn1_create_octet_string(encrypted_pdu, encrypted_len);
    } else {
        msg_data = scoped_pdu;
        scoped_pdu = NULL;
    }
    if (!msg_data) return SNMP_BUILD_ERROR_MEMORY;

    params_copy = *usm_params;
    memcpy(params_copy.priv_params, priv_params, sizeof(priv_params));
    memset(params_copy.auth_params, 0, sizeof(params_copy.auth_params));
    result = snmp_usm_encode_security_params_sized(
        &params_copy,
        security_level >= SNMP_SEC_LEVEL_AUTH_NOPRIV
            ? sizeof(params_copy.auth_params)
            : 0u,
        security_level == SNMP_SEC_LEVEL_AUTH_PRIV
            ? sizeof(params_copy.priv_params)
            : 0u,
        security_params, &security_params_len);
    if (result != USM_OK) {
        asn1_free(msg_data);
        return SNMP_BUILD_ERROR_INVALID;
    }

    global_data = asn1_create_sequence();
    msg_flags = reportable ? SNMP_MSG_FLAG_REPORTABLE : 0u;
    if (security_level >= SNMP_SEC_LEVEL_AUTH_NOPRIV) msg_flags |= SNMP_MSG_FLAG_AUTH;
    if (security_level == SNMP_SEC_LEVEL_AUTH_PRIV) msg_flags |= SNMP_MSG_FLAG_PRIV;
    if (!global_data ||
        snmp_builder_add_child(global_data, asn1_create_integer(request_id)) < 0 ||
        snmp_builder_add_child(global_data, asn1_create_integer(65507)) < 0 ||
        snmp_builder_add_child(
            global_data, asn1_create_octet_string(&msg_flags, 1u)) < 0 ||
        snmp_builder_add_child(global_data, asn1_create_integer(3)) < 0) {
        asn1_free(global_data);
        asn1_free(msg_data);
        return SNMP_BUILD_ERROR_MEMORY;
    }

    msg = asn1_create_sequence();
    if (!msg) {
        asn1_free(global_data);
        asn1_free(msg_data);
        return SNMP_BUILD_ERROR_MEMORY;
    }
    if (snmp_builder_add_child(msg, asn1_create_integer(3)) < 0) {
        asn1_free(msg);
        asn1_free(global_data);
        asn1_free(msg_data);
        return SNMP_BUILD_ERROR_MEMORY;
    }
    if (snmp_builder_add_child(msg, global_data) < 0) {
        asn1_free(msg);
        asn1_free(msg_data);
        return SNMP_BUILD_ERROR_MEMORY;
    }
    global_data = NULL;
    if (snmp_builder_add_child(
            msg, asn1_create_octet_string(security_params,
                                          security_params_len)) < 0) {
        asn1_free(msg);
        asn1_free(msg_data);
        return SNMP_BUILD_ERROR_MEMORY;
    }
    if (snmp_builder_add_child(msg, msg_data) < 0) {
        asn1_free(msg);
        return SNMP_BUILD_ERROR_MEMORY;
    }
    msg_data = NULL;

    result = asn1_der_encode(msg, out, out_len);
    if (result != 0) {
        asn1_free(msg);
        return SNMP_BUILD_ERROR_BUFFER;
    }

    if (security_level >= SNMP_SEC_LEVEL_AUTH_NOPRIV) {
        uint8_t auth_params[12];
        result = usm_compute_auth(out, *out_len, user->auth_key, user->auth_key_len,
                                  user->auth_protocol, auth_params);
        const uint8_t *auth_field = NULL;
        size_t auth_field_len = 0u;
        if (result != USM_OK ||
            snmp_usm_find_auth_field(out, *out_len, &auth_field, &auth_field_len) != 0 ||
            auth_field_len != sizeof(auth_params)) {
            asn1_free(msg);
            return SNMP_BUILD_ERROR_INVALID;
        }
        memcpy(out + (size_t)(auth_field - out), auth_params, sizeof(auth_params));
    }

    asn1_free(msg);
    return SNMP_BUILD_OK;
}

int snmp_build_v3_query(
    snmp_pdu_type_t pdu_type, int32_t request_id, const snmp_oid_t *oids,
    size_t oid_count, const snmp_usm_params_t *usm_params,
    const snmp_v3_user_t *user, snmp_security_level_t security_level,
    uint8_t *out, size_t *out_len) {
    asn1_value_t *pdu = NULL;
    int result;
    if (!oids || oid_count == 0u ||
        (pdu_type != SNMP_PDU_GET_REQUEST &&
         pdu_type != SNMP_PDU_GET_NEXT_REQUEST &&
         pdu_type != SNMP_PDU_GET_RESPONSE && pdu_type != SNMP_PDU_REPORT)) {
        return SNMP_BUILD_ERROR_INVALID;
    }
    result = build_get_pdu(pdu_type, request_id, oids, oid_count, &pdu);
    if (result != SNMP_BUILD_OK) return result;
    return snmp_build_v3_pdu(
        pdu, request_id,
        pdu_type == SNMP_PDU_GET_REQUEST || pdu_type == SNMP_PDU_GET_NEXT_REQUEST,
        usm_params, user, security_level, out, out_len);
}

int snmp_build_v3_set_request(
    int32_t request_id, const snmp_varbind_t *varbinds, size_t varbind_count,
    const snmp_usm_params_t *usm_params, const snmp_v3_user_t *user,
    snmp_security_level_t security_level, uint8_t *out, size_t *out_len) {
    asn1_value_t *pdu = NULL;
    int result = build_set_pdu(request_id, varbinds, varbind_count, &pdu);
    if (result != SNMP_BUILD_OK) return result;
    return snmp_build_v3_pdu(pdu, request_id, 1, usm_params, user,
                             security_level, out, out_len);
}

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
    return snmp_build_v3_query(SNMP_PDU_GET_REQUEST, request_id, oids,
                               oid_count, usm_params, user, security_level, out,
                               out_len);
}

int snmp_build_v3_get_next_request(
    int32_t request_id,
    const snmp_oid_t *oids,
    size_t oid_count,
    const snmp_usm_params_t *usm_params,
    const snmp_v3_user_t *user,
    snmp_security_level_t security_level,
    uint8_t *out,
    size_t *out_len
) {
    return snmp_build_v3_query(SNMP_PDU_GET_NEXT_REQUEST, request_id, oids,
                               oid_count, usm_params, user, security_level, out,
                               out_len);
}
