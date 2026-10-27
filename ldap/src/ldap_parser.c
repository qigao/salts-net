/**
 * @file ldap_parser.c
 * @brief LDAP Message Parser - parses LDAP protocol responses from ASN.1 BER
 */

#include "ldap_parser.h"
#include <salts/asn1_value.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Check if we have a complete BER TLV */
int ldap_message_complete(const uint8_t *data, size_t len) {
    salts_asn1_reader reader;
    salts_asn1_tlv value;
    salts_asn1_status status;
    if (!data && len != 0u) return -1;
    if (salts_asn1_reader_init(&reader, data, len, SALTS_ASN1_BER) !=
        SALTS_ASN1_OK)
        return -1;
    status = salts_asn1_reader_next(&reader, &value);
    if (status == SALTS_ASN1_UNEXPECTED_END || status == SALTS_ASN1_DONE)
        return 0;
    if (status != SALTS_ASN1_OK || value.encoded_size > INT_MAX) return -1;
    return (int)value.encoded_size;
}

static int tag_is(const salts_asn1_value *value, uint8_t tag_class,
                  uint8_t constructed, uint32_t tag_number) {
    uint8_t actual_class = 0u, actual_constructed = 0u;
    uint32_t actual_number = 0u;
    return value &&
           salts_asn1_value_get_tag(value, &actual_class, &actual_constructed,
                                    &actual_number) == SALTS_ASN1_OK &&
           actual_class == tag_class && actual_constructed == constructed &&
           actual_number == tag_number;
}

/* Extract OCTET STRING as null-terminated string */
static char *extract_string(const salts_asn1_value *value) {
    const uint8_t *data = NULL;
    size_t len = 0u;
    if (!tag_is(value, 0u, 0u, 4u) ||
        salts_asn1_value_get_content(value, &data, &len) != SALTS_ASN1_OK)
        return NULL;
    char *str = malloc(len + 1);
    if (str) {
        if (len > 0) memcpy(str, data, len);
        str[len] = '\0';
    }
    return str;
}

/* Extract OCTET STRING data (copy) */
static uint8_t *extract_data(const salts_asn1_value *value, size_t *out_len) {
    const uint8_t *content = NULL;
    size_t len = 0u;
    if (!out_len || !tag_is(value, 0u, 0u, 4u) ||
        salts_asn1_value_get_content(value, &content, &len) != SALTS_ASN1_OK) {
        if (!out_len) return NULL;
        *out_len = 0;
        return NULL;
    }
    uint8_t *data = len ? malloc(len) : NULL;
    if (data && len > 0) {
        memcpy(data, content, len);
    }
    *out_len = len;
    return data;
}

/* Get child from sequence by index */
static const salts_asn1_value *seq_child(const salts_asn1_value *seq,
                                         size_t idx) {
    return salts_asn1_value_child_at(seq, idx);
}

/* Get sequence child count */
static size_t seq_count(const salts_asn1_value *seq) {
    return salts_asn1_value_child_count(seq);
}

static int get_enumerated(const salts_asn1_value *value, int *out) {
    const uint8_t *data = NULL;
    size_t size = 0u;
    uint64_t result = 0u;
    if (!out || !tag_is(value, 0u, 0u, 10u) ||
        salts_asn1_value_get_content(value, &data, &size) != SALTS_ASN1_OK ||
        size == 0u || size > sizeof(int) || (data[0] & 0x80u) != 0u)
        return -1;
    for (size_t i = 0u; i < size; ++i) result = (result << 8u) | data[i];
    if (result > INT_MAX) return -1;
    *out = (int)result;
    return 0;
}

/* Parse LDAPResult: resultCode, matchedDN, diagnosticMessage, [referral] */
static int parse_ldap_result(const salts_asn1_value *seq, size_t start_idx,
                             ldap_result_data_t *result) {
    memset(result, 0, sizeof(*result));

    /* resultCode (INTEGER/ENUMERATED) */
    const salts_asn1_value *code = seq_child(seq, start_idx);
    if (!code || get_enumerated(code, &result->result_code) != 0)
        return LDAP_PARSE_ERROR_FORMAT;

    /* matchedDN (OCTET STRING) */
    const salts_asn1_value *matched = seq_child(seq, start_idx + 1);
    if (matched) {
        result->matched_dn = extract_string(matched);
    }

    /* diagnosticMessage (OCTET STRING) */
    const salts_asn1_value *diag = seq_child(seq, start_idx + 2);
    if (diag) {
        result->diagnostic_message = extract_string(diag);
    }

    /* referral [3] SEQUENCE OF URI (optional) */
    if (seq_count(seq) > start_idx + 3) {
        const salts_asn1_value *ref = seq_child(seq, start_idx + 3);
        if (tag_is(ref, 2u, 1u, 3u)) {
            size_t ref_count = seq_count(ref);
            result->referrals = malloc(ref_count * sizeof(char *));
            if (result->referrals) {
                result->referral_count = ref_count;
                for (size_t i = 0; i < ref_count; i++) {
                    result->referrals[i] = extract_string(seq_child(ref, i));
                }
            }
        }
    }

    return LDAP_PARSE_OK;
}

/* Parse SearchResultEntry */
static int parse_search_entry(const salts_asn1_value *entry_seq,
                              ldap_entry_t *entry) {
    memset(entry, 0, sizeof(*entry));

    /* objectName (DN) */
    const salts_asn1_value *dn = seq_child(entry_seq, 0);
    if (!dn) return LDAP_PARSE_ERROR_FORMAT;
    entry->dn = extract_string(dn);

    /* attributes (PartialAttributeList) */
    const salts_asn1_value *attrs = seq_child(entry_seq, 1);
    if (!attrs) return LDAP_PARSE_OK;  /* No attributes */

    size_t attr_count = seq_count(attrs);
    if (attr_count == 0) return LDAP_PARSE_OK;

    entry->attributes = calloc(attr_count, sizeof(ldap_attribute_t));
    if (!entry->attributes) return LDAP_PARSE_ERROR_MEMORY;
    entry->attribute_count = attr_count;

    for (size_t i = 0; i < attr_count; i++) {
        const salts_asn1_value *attr_seq = seq_child(attrs, i);
        if (!attr_seq) continue;

        /* type (AttributeDescription) */
        const salts_asn1_value *type = seq_child(attr_seq, 0);
        if (type) {
            entry->attributes[i].type = extract_string(type);
        }

        /* vals (SET OF AttributeValue) */
        const salts_asn1_value *vals = seq_child(attr_seq, 1);
        if (tag_is(vals, 0u, 1u, 17u)) {
            size_t val_count = seq_count(vals);
            if (val_count > 0) {
                ldap_value_t *values = calloc(val_count, sizeof(ldap_value_t));
                if (values) {
                    for (size_t j = 0; j < val_count; j++) {
                        const salts_asn1_value *v = seq_child(vals, j);
                        if (v) {
                            values[j].data = extract_data(v, &values[j].length);
                        }
                    }
                    entry->attributes[i].values = values;
                    entry->attributes[i].value_count = val_count;
                }
            }
        }
    }

    return LDAP_PARSE_OK;
}

/* Main parse function */
int ldap_parse_message(const uint8_t *data, size_t len, ldap_parse_result_t *result) {
    if (!data || !result) return LDAP_PARSE_ERROR_INVALID;

    memset(result, 0, sizeof(*result));

    /* Check completeness */
    int complete = ldap_message_complete(data, len);
    if (complete <= 0) return LDAP_PARSE_ERROR_TRUNCATED;

    /* Decode BER */
    salts_asn1_value *root = NULL;
    if (salts_asn1_value_decode(data, (size_t)complete, SALTS_ASN1_BER,
                                &root) != SALTS_ASN1_OK)
        return LDAP_PARSE_ERROR_FORMAT;

    result->bytes_consumed = complete;

    /* LDAPMessage ::= SEQUENCE { messageID, protocolOp, [controls] } */
    if (!tag_is(root, 0u, 1u, 16u) || seq_count(root) < 2) {
        salts_asn1_value_free(root);
        return LDAP_PARSE_ERROR_FORMAT;
    }

    /* Allocate message */
    ldap_message_t *msg = calloc(1, sizeof(ldap_message_t));
    if (!msg) {
        salts_asn1_value_free(root);
        return LDAP_PARSE_ERROR_MEMORY;
    }

    /* messageID */
    const salts_asn1_value *mid = seq_child(root, 0);
    int64_t message_id = 0;
    if (salts_asn1_value_get_integer_i64(mid, &message_id) != SALTS_ASN1_OK ||
        message_id < 0 || message_id > INT_MAX) {
        free(msg);
        salts_asn1_value_free(root);
        return LDAP_PARSE_ERROR_FORMAT;
    }
    msg->message_id = (int)message_id;

    /* protocolOp */
    const salts_asn1_value *op = seq_child(root, 1);
    if (!op) {
        free(msg);
        salts_asn1_value_free(root);
        return LDAP_PARSE_ERROR_FORMAT;
    }
    uint8_t op_class = 0u, op_constructed = 0u;
    uint32_t op_number = 0u;
    if (salts_asn1_value_get_tag(op, &op_class, &op_constructed, &op_number) !=
            SALTS_ASN1_OK ||
        op_class != 1u) {
        free(msg);
        salts_asn1_value_free(root);
        return LDAP_PARSE_ERROR_FORMAT;
    }
    msg->protocol_op = (int)op_number;

    int parse_rc = LDAP_PARSE_OK;

    switch (op_number) {
        case LDAP_RES_BIND:
            /* BindResponse: resultCode, matchedDN, diagnosticMessage, [serverSaslCreds] */
            parse_rc = parse_ldap_result(op, 0, &msg->payload.bind_response);
            break;

        case LDAP_RES_SEARCH_ENTRY:
            /* SearchResultEntry: objectName, attributes */
            parse_rc = parse_search_entry(op, &msg->payload.search_entry);
            break;

        case LDAP_RES_SEARCH_DONE:
        case LDAP_RES_MODIFY:
        case LDAP_RES_ADD:
        case LDAP_RES_DELETE:
        case LDAP_RES_MODDN:
        case LDAP_RES_COMPARE:
            /* All use LDAPResult format */
            parse_rc = parse_ldap_result(op, 0, &msg->payload.generic_result);
            break;

        case LDAP_RES_SEARCH_REF:
            /* SearchResultReference: SEQUENCE OF URI - store in referrals */
            msg->payload.generic_result.referral_count = seq_count(op);
            if (msg->payload.generic_result.referral_count > 0) {
                msg->payload.generic_result.referrals = malloc(
                    msg->payload.generic_result.referral_count * sizeof(char *));
                if (msg->payload.generic_result.referrals) {
                    for (size_t i = 0; i < msg->payload.generic_result.referral_count; i++) {
                        msg->payload.generic_result.referrals[i] = extract_string(seq_child(op, i));
                    }
                }
            }
            break;

        default:
            /* Unknown op - just store the tag */
            break;
    }

    salts_asn1_value_free(root);

    if (parse_rc != LDAP_PARSE_OK) {
        ldap_message_free(msg);
        return parse_rc;
    }

    result->message = msg;
    return LDAP_PARSE_OK;
}

void ldap_result_free(ldap_result_data_t *result) {
    if (!result) return;
    free(result->matched_dn);
    free(result->diagnostic_message);
    if (result->referrals) {
        for (size_t i = 0; i < result->referral_count; i++) {
            free(result->referrals[i]);
        }
        free(result->referrals);
    }
    memset(result, 0, sizeof(*result));
}

void ldap_entry_free(ldap_entry_t *entry) {
    if (!entry) return;
    free(entry->dn);
    if (entry->attributes) {
        for (size_t i = 0; i < entry->attribute_count; i++) {
            free((void *)entry->attributes[i].type);
            if (entry->attributes[i].values) {
                for (size_t j = 0; j < entry->attributes[i].value_count; j++) {
                    free(entry->attributes[i].values[j].data);
                }
                free((void *)entry->attributes[i].values);
            }
        }
        free(entry->attributes);
    }
    memset(entry, 0, sizeof(*entry));
}

void ldap_message_free(ldap_message_t *msg) {
    if (!msg) return;

    switch (msg->protocol_op) {
        case LDAP_RES_BIND:
            ldap_result_free(&msg->payload.bind_response);
            break;
        case LDAP_RES_SEARCH_ENTRY:
            ldap_entry_free(&msg->payload.search_entry);
            break;
        case LDAP_RES_SEARCH_DONE:
        case LDAP_RES_SEARCH_REF:
        case LDAP_RES_MODIFY:
        case LDAP_RES_ADD:
        case LDAP_RES_DELETE:
        case LDAP_RES_MODDN:
        case LDAP_RES_COMPARE:
            ldap_result_free(&msg->payload.generic_result);
            break;
        default:
            break;
    }

    if (msg->controls) {
        for (size_t i = 0; i < msg->control_count; i++) {
            free(msg->controls[i].oid);
            free(msg->controls[i].value.data);
        }
        free(msg->controls);
    }

    free(msg);
}

const char *ldap_result_code_str(int code) {
    switch (code) {
        case LDAP_SUCCESS:                    return "success";
        case LDAP_OPERATIONS_ERROR:           return "operationsError";
        case LDAP_PROTOCOL_ERROR:             return "protocolError";
        case LDAP_TIMELIMIT_EXCEEDED:         return "timeLimitExceeded";
        case LDAP_SIZELIMIT_EXCEEDED:         return "sizeLimitExceeded";
        case LDAP_COMPARE_FALSE:              return "compareFalse";
        case LDAP_COMPARE_TRUE:               return "compareTrue";
        case LDAP_AUTH_METHOD_NOT_SUPPORTED:  return "authMethodNotSupported";
        case LDAP_STRONG_AUTH_REQUIRED:       return "strongAuthRequired";
        case LDAP_REFERRAL:                   return "referral";
        case LDAP_ADMIN_LIMIT_EXCEEDED:       return "adminLimitExceeded";
        case LDAP_UNAVAILABLE_CRITICAL_EXTENSION: return "unavailableCriticalExtension";
        case LDAP_CONFIDENTIALITY_REQUIRED:   return "confidentialityRequired";
        case LDAP_SASL_BIND_IN_PROGRESS:      return "saslBindInProgress";
        case LDAP_NO_SUCH_ATTRIBUTE:          return "noSuchAttribute";
        case LDAP_UNDEFINED_TYPE:             return "undefinedAttributeType";
        case LDAP_INAPPROPRIATE_MATCHING:     return "inappropriateMatching";
        case LDAP_CONSTRAINT_VIOLATION:       return "constraintViolation";
        case LDAP_TYPE_OR_VALUE_EXISTS:       return "attributeOrValueExists";
        case LDAP_INVALID_ATTRIBUTE_SYNTAX:   return "invalidAttributeSyntax";
        case LDAP_NO_SUCH_OBJECT:             return "noSuchObject";
        case LDAP_ALIAS_PROBLEM:              return "aliasProblem";
        case LDAP_INVALID_DN_SYNTAX:          return "invalidDNSyntax";
        case LDAP_ALIAS_DEREF_PROBLEM:        return "aliasDereferencingProblem";
        case LDAP_INAPPROPRIATE_AUTH:         return "inappropriateAuthentication";
        case LDAP_INVALID_CREDENTIALS:        return "invalidCredentials";
        case LDAP_INSUFFICIENT_ACCESS:        return "insufficientAccessRights";
        case LDAP_BUSY:                       return "busy";
        case LDAP_UNAVAILABLE:                return "unavailable";
        case LDAP_UNWILLING_TO_PERFORM:       return "unwillingToPerform";
        case LDAP_LOOP_DETECT:                return "loopDetect";
        case LDAP_NAMING_VIOLATION:           return "namingViolation";
        case LDAP_OBJECT_CLASS_VIOLATION:     return "objectClassViolation";
        case LDAP_NOT_ALLOWED_ON_NONLEAF:     return "notAllowedOnNonLeaf";
        case LDAP_NOT_ALLOWED_ON_RDN:         return "notAllowedOnRDN";
        case LDAP_ENTRY_ALREADY_EXISTS:       return "entryAlreadyExists";
        case LDAP_OBJECT_CLASS_MODS_PROHIBITED: return "objectClassModsProhibited";
        case LDAP_AFFECTS_MULTIPLE_DSAS:      return "affectsMultipleDSAs";
        case LDAP_OTHER:                      return "other";
        default:                              return "unknown";
    }
}
