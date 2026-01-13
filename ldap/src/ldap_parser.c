/**
 * @file ldap_parser.c
 * @brief LDAP Message Parser - parses LDAP protocol responses from ASN.1 BER
 */

#include "ldap_parser.h"
#include "asn1_types.h"
#include <stdlib.h>
#include <string.h>

/* Check if we have a complete BER TLV */
int ldap_message_complete(const uint8_t *data, size_t len) {
    if (len < 2) return 0;

    /* Skip tag byte */
    size_t pos = 1;

    /* Parse length */
    if (data[pos] < 0x80) {
        /* Short form */
        size_t content_len = data[pos];
        size_t total = pos + 1 + content_len;
        return (len >= total) ? (int)total : 0;
    } else if (data[pos] == 0x80) {
        /* Indefinite length - scan for end-of-contents */
        return -1;  /* Not supported for now */
    } else {
        /* Long form */
        int num_octets = data[pos] & 0x7f;
        pos++;
        if (len < pos + num_octets) return 0;

        size_t content_len = 0;
        for (int i = 0; i < num_octets; i++) {
            content_len = (content_len << 8) | data[pos + i];
        }
        size_t total = pos + num_octets + content_len;
        return (len >= total) ? (int)total : 0;
    }
}

/* Extract OCTET STRING as null-terminated string */
static char *extract_string(const asn1_value_t *val) {
    if (!val || val->type != ASN1_TYPE_OCTET_STRING) return NULL;

    size_t len = val->value.octet_string.length;
    char *str = malloc(len + 1);
    if (str) {
        if (len > 0) memcpy(str, val->value.octet_string.data, len);
        str[len] = '\0';
    }
    return str;
}

/* Extract OCTET STRING data (copy) */
static uint8_t *extract_data(const asn1_value_t *val, size_t *out_len) {
    if (!val || val->type != ASN1_TYPE_OCTET_STRING) {
        *out_len = 0;
        return NULL;
    }

    size_t len = val->value.octet_string.length;
    uint8_t *data = malloc(len);
    if (data && len > 0) {
        memcpy(data, val->value.octet_string.data, len);
    }
    *out_len = len;
    return data;
}

/* Get child from sequence by index */
static asn1_value_t *seq_child(const asn1_value_t *seq, size_t idx) {
    if (!seq || seq->type != ASN1_TYPE_SEQUENCE) return NULL;
    if (idx >= seq->value.sequence.count) return NULL;
    return seq->value.sequence.children[idx];
}

/* Get sequence child count */
static size_t seq_count(const asn1_value_t *seq) {
    if (!seq || seq->type != ASN1_TYPE_SEQUENCE) return 0;
    return seq->value.sequence.count;
}

/* Parse LDAPResult: resultCode, matchedDN, diagnosticMessage, [referral] */
static int parse_ldap_result(const asn1_value_t *seq, size_t start_idx, ldap_result_data_t *result) {
    memset(result, 0, sizeof(*result));

    /* resultCode (INTEGER/ENUMERATED) */
    asn1_value_t *code = seq_child(seq, start_idx);
    if (!code) return LDAP_PARSE_ERROR_FORMAT;

    /* ENUMERATED (tag 0x0a) is encoded same as INTEGER but may be stored differently */
    if (code->type == ASN1_TYPE_INTEGER || code->tag == 0x0a) {
        result->result_code = (int)code->value.integer;
    } else if (code->type == ASN1_TYPE_OCTET_STRING && code->value.octet_string.length > 0) {
        /* Fallback: parse from raw bytes */
        result->result_code = 0;
        for (size_t i = 0; i < code->value.octet_string.length; i++) {
            result->result_code = (result->result_code << 8) | code->value.octet_string.data[i];
        }
    }

    /* matchedDN (OCTET STRING) */
    asn1_value_t *matched = seq_child(seq, start_idx + 1);
    if (matched) {
        result->matched_dn = extract_string(matched);
    }

    /* diagnosticMessage (OCTET STRING) */
    asn1_value_t *diag = seq_child(seq, start_idx + 2);
    if (diag) {
        result->diagnostic_message = extract_string(diag);
    }

    /* referral [3] SEQUENCE OF URI (optional) */
    if (seq_count(seq) > start_idx + 3) {
        asn1_value_t *ref = seq_child(seq, start_idx + 3);
        if (ref && ref->tag_class == 2 && ref->tag_number == 3) {
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
static int parse_search_entry(const asn1_value_t *entry_seq, ldap_entry_t *entry) {
    memset(entry, 0, sizeof(*entry));

    /* objectName (DN) */
    asn1_value_t *dn = seq_child(entry_seq, 0);
    if (!dn) return LDAP_PARSE_ERROR_FORMAT;
    entry->dn = extract_string(dn);

    /* attributes (PartialAttributeList) */
    asn1_value_t *attrs = seq_child(entry_seq, 1);
    if (!attrs) return LDAP_PARSE_OK;  /* No attributes */

    size_t attr_count = seq_count(attrs);
    if (attr_count == 0) return LDAP_PARSE_OK;

    entry->attributes = calloc(attr_count, sizeof(ldap_attribute_t));
    if (!entry->attributes) return LDAP_PARSE_ERROR_MEMORY;
    entry->attribute_count = attr_count;

    for (size_t i = 0; i < attr_count; i++) {
        asn1_value_t *attr_seq = seq_child(attrs, i);
        if (!attr_seq) continue;

        /* type (AttributeDescription) */
        asn1_value_t *type = seq_child(attr_seq, 0);
        if (type) {
            entry->attributes[i].type = extract_string(type);
        }

        /* vals (SET OF AttributeValue) */
        asn1_value_t *vals = seq_child(attr_seq, 1);
        if (vals && vals->type == ASN1_TYPE_SET) {
            size_t val_count = vals->value.set.count;
            if (val_count > 0) {
                ldap_value_t *values = calloc(val_count, sizeof(ldap_value_t));
                if (values) {
                    for (size_t j = 0; j < val_count; j++) {
                        asn1_value_t *v = vals->value.set.children[j];
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
    asn1_value_t *root = NULL;
    int rc = asn1_ber_decode(data, complete, &root);
    if (rc != 0 || !root) return LDAP_PARSE_ERROR_FORMAT;

    result->bytes_consumed = complete;

    /* LDAPMessage ::= SEQUENCE { messageID, protocolOp, [controls] } */
    if (root->type != ASN1_TYPE_SEQUENCE || seq_count(root) < 2) {
        asn1_free(root);
        return LDAP_PARSE_ERROR_FORMAT;
    }

    /* Allocate message */
    ldap_message_t *msg = calloc(1, sizeof(ldap_message_t));
    if (!msg) {
        asn1_free(root);
        return LDAP_PARSE_ERROR_MEMORY;
    }

    /* messageID */
    asn1_value_t *mid = seq_child(root, 0);
    if (mid) {
        msg->message_id = (int)mid->value.integer;
    }

    /* protocolOp */
    asn1_value_t *op = seq_child(root, 1);
    if (!op) {
        free(msg);
        asn1_free(root);
        return LDAP_PARSE_ERROR_FORMAT;
    }

    msg->protocol_op = op->tag_number;

    int parse_rc = LDAP_PARSE_OK;

    switch (op->tag_number) {
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

    asn1_free(root);

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
