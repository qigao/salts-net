/**
 * @file ldap_builder.c
 * @brief LDAP Message Builder - constructs LDAP protocol messages using ASN.1 BER
 */

#include "ldap_builder.h"
#include "asn1_types.h"
#include <stdlib.h>
#include <string.h>

/* Forward declarations for filter parsing */
static asn1_value_t *parse_filter(const char **p);

/* Create APPLICATION tagged sequence */
static asn1_value_t *create_app_sequence(int tag_num) {
    asn1_value_t *seq = asn1_create_sequence();
    if (seq) {
        seq->tag_class = 1;  /* APPLICATION */
        seq->tag_number = tag_num;
        seq->tag = 0x60 | tag_num;  /* APPLICATION CONSTRUCTED */
    }
    return seq;
}

/* Create CONTEXT-SPECIFIC primitive */
static asn1_value_t *create_ctx_primitive(int tag_num, const uint8_t *data, size_t len) {
    asn1_value_t *val = asn1_create_octet_string(data, len);
    if (val) {
        val->tag_class = 2;  /* CONTEXT-SPECIFIC */
        val->tag_number = tag_num;
        val->constructed = 0;
        val->tag = 0x80 | tag_num;
    }
    return val;
}

/* Create CONTEXT-SPECIFIC constructed (sequence) */
static asn1_value_t *create_ctx_constructed(int tag_num) {
    asn1_value_t *seq = asn1_create_sequence();
    if (seq) {
        seq->tag_class = 2;  /* CONTEXT-SPECIFIC */
        seq->tag_number = tag_num;
        seq->tag = 0xa0 | tag_num;
    }
    return seq;
}

/* Wrap protocolOp in LDAPMessage */
static int wrap_ldap_message(int message_id, asn1_value_t *op, uint8_t *out, size_t *out_len) {
    asn1_value_t *msg = asn1_create_sequence();
    if (!msg) {
        asn1_free(op);
        return LDAP_BUILD_ERROR_MEMORY;
    }

    asn1_value_t *mid = asn1_create_integer(message_id);
    if (!mid || asn1_sequence_add_child(msg, mid) < 0) {
        asn1_free(msg);
        asn1_free(op);
        return LDAP_BUILD_ERROR_MEMORY;
    }

    if (asn1_sequence_add_child(msg, op) < 0) {
        asn1_free(msg);
        return LDAP_BUILD_ERROR_MEMORY;
    }

    int result = asn1_ber_encode(msg, out, out_len, 0);
    asn1_free(msg);
    return (result == 0) ? LDAP_BUILD_OK : LDAP_BUILD_ERROR_BUFFER;
}

/*
 * BindRequest ::= [APPLICATION 0] SEQUENCE {
 *     version        INTEGER (1..127),
 *     name           LDAPDN,
 *     authentication AuthenticationChoice
 * }
 * AuthenticationChoice ::= CHOICE {
 *     simple [0] OCTET STRING,
 *     ...
 * }
 */
int ldap_build_bind_request(
    int message_id,
    int version,
    const char *dn,
    const char *password,
    uint8_t *out,
    size_t *out_len
) {
    if (!out || !out_len) return LDAP_BUILD_ERROR_INVALID;

    asn1_value_t *bind = create_app_sequence(LDAP_REQ_BIND);
    if (!bind) return LDAP_BUILD_ERROR_MEMORY;

    /* version */
    asn1_value_t *ver = asn1_create_integer(version);
    if (!ver || asn1_sequence_add_child(bind, ver) < 0) {
        asn1_free(bind);
        return LDAP_BUILD_ERROR_MEMORY;
    }

    /* name (DN) */
    const char *dn_str = dn ? dn : "";
    asn1_value_t *name = asn1_create_octet_string((const uint8_t *)dn_str, strlen(dn_str));
    if (!name || asn1_sequence_add_child(bind, name) < 0) {
        asn1_free(bind);
        return LDAP_BUILD_ERROR_MEMORY;
    }

    /* authentication: simple [0] */
    const char *pwd = password ? password : "";
    asn1_value_t *auth = create_ctx_primitive(0, (const uint8_t *)pwd, strlen(pwd));
    if (!auth || asn1_sequence_add_child(bind, auth) < 0) {
        asn1_free(bind);
        return LDAP_BUILD_ERROR_MEMORY;
    }

    return wrap_ldap_message(message_id, bind, out, out_len);
}

/*
 * UnbindRequest ::= [APPLICATION 2] NULL
 */
int ldap_build_unbind_request(
    int message_id,
    uint8_t *out,
    size_t *out_len
) {
    if (!out || !out_len) return LDAP_BUILD_ERROR_INVALID;

    asn1_value_t *unbind = asn1_create_null();
    if (!unbind) return LDAP_BUILD_ERROR_MEMORY;

    unbind->tag_class = 1;  /* APPLICATION */
    unbind->tag_number = LDAP_REQ_UNBIND;
    unbind->tag = 0x42;  /* APPLICATION 2 primitive */

    return wrap_ldap_message(message_id, unbind, out, out_len);
}

/* ============================================================================
 * Filter Parser - converts "(cn=foo)" style strings to ASN.1
 * ============================================================================ */

static void skip_spaces(const char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

static char *parse_attr_desc(const char **p) {
    const char *start = *p;
    while (**p && **p != '=' && **p != '>' && **p != '<' && **p != '~' &&
           **p != ')' && **p != '(' && **p != '*') {
        (*p)++;
    }
    size_t len = *p - start;
    char *attr = malloc(len + 1);
    if (attr) {
        memcpy(attr, start, len);
        attr[len] = '\0';
    }
    return attr;
}

static uint8_t *parse_value(const char **p, size_t *out_len) {
    const char *start = *p;
    while (**p && **p != ')' && **p != '*') (*p)++;
    size_t len = *p - start;
    uint8_t *val = malloc(len + 1);
    if (val) {
        memcpy(val, start, len);
        val[len] = '\0';
        *out_len = len;
    }
    return val;
}

/* AttributeValueAssertion ::= SEQUENCE { attributeDesc, assertionValue } */
static asn1_value_t *create_ava(const char *attr, const uint8_t *val, size_t val_len) {
    asn1_value_t *ava = asn1_create_sequence();
    if (!ava) return NULL;

    asn1_value_t *attr_val = asn1_create_octet_string((const uint8_t *)attr, strlen(attr));
    asn1_value_t *assert_val = asn1_create_octet_string(val, val_len);

    if (!attr_val || !assert_val ||
        asn1_sequence_add_child(ava, attr_val) < 0 ||
        asn1_sequence_add_child(ava, assert_val) < 0) {
        asn1_free(ava);
        asn1_free(attr_val);
        asn1_free(assert_val);
        return NULL;
    }
    return ava;
}

/* Parse simple filter item: attr=val, attr>=val, attr<=val, attr~=val, attr=* */
static asn1_value_t *parse_filter_item(const char **p) {
    char *attr = parse_attr_desc(p);
    if (!attr) return NULL;

    int filter_type;
    if (**p == '=' && *(*p + 1) != '*') {
        (*p)++;
        filter_type = 3;  /* equalityMatch */
    } else if (**p == '>' && *(*p + 1) == '=') {
        (*p) += 2;
        filter_type = 5;  /* greaterOrEqual */
    } else if (**p == '<' && *(*p + 1) == '=') {
        (*p) += 2;
        filter_type = 6;  /* lessOrEqual */
    } else if (**p == '~' && *(*p + 1) == '=') {
        (*p) += 2;
        filter_type = 8;  /* approxMatch */
    } else if (**p == '=' && *(*p + 1) == '*') {
        /* present filter: attr=* */
        (*p) += 2;
        asn1_value_t *present = asn1_create_octet_string((const uint8_t *)attr, strlen(attr));
        free(attr);
        if (present) {
            present->tag_class = 2;
            present->tag_number = 7;
            present->tag = 0x87;  /* context [7] primitive */
        }
        return present;
    } else {
        free(attr);
        return NULL;
    }

    size_t val_len;
    uint8_t *val = parse_value(p, &val_len);
    if (!val) {
        free(attr);
        return NULL;
    }

    asn1_value_t *ava = create_ava(attr, val, val_len);
    free(attr);
    free(val);

    if (!ava) return NULL;

    /* Set context-specific tag */
    ava->tag_class = 2;
    ava->tag_number = filter_type;
    ava->tag = 0xa0 | filter_type;

    return ava;
}

/* Parse compound filter: &(...), |(...), !(...) */
static asn1_value_t *parse_filter(const char **p) {
    skip_spaces(p);
    if (**p != '(') return NULL;
    (*p)++;
    skip_spaces(p);

    asn1_value_t *filter = NULL;

    if (**p == '&' || **p == '|') {
        int is_and = (**p == '&');
        (*p)++;

        filter = create_ctx_constructed(is_and ? 0 : 1);
        if (!filter) return NULL;

        while (**p == '(') {
            asn1_value_t *child = parse_filter(p);
            if (!child || asn1_sequence_add_child(filter, child) < 0) {
                asn1_free(filter);
                return NULL;
            }
            skip_spaces(p);
        }
    } else if (**p == '!') {
        (*p)++;
        filter = create_ctx_constructed(2);
        if (!filter) return NULL;

        asn1_value_t *child = parse_filter(p);
        if (!child || asn1_sequence_add_child(filter, child) < 0) {
            asn1_free(filter);
            return NULL;
        }
    } else {
        filter = parse_filter_item(p);
    }

    skip_spaces(p);
    if (**p == ')') (*p)++;

    return filter;
}

/*
 * SearchRequest ::= [APPLICATION 3] SEQUENCE {
 *     baseObject   LDAPDN,
 *     scope        ENUMERATED,
 *     derefAliases ENUMERATED,
 *     sizeLimit    INTEGER,
 *     timeLimit    INTEGER,
 *     typesOnly    BOOLEAN,
 *     filter       Filter,
 *     attributes   AttributeSelection
 * }
 */
int ldap_build_search_request(
    int message_id,
    const char *base_dn,
    int scope,
    int deref_aliases,
    int size_limit,
    int time_limit,
    int types_only,
    const char *filter_str,
    const char **attributes,
    uint8_t *out,
    size_t *out_len
) {
    if (!out || !out_len) return LDAP_BUILD_ERROR_INVALID;

    asn1_value_t *search = create_app_sequence(LDAP_REQ_SEARCH);
    if (!search) return LDAP_BUILD_ERROR_MEMORY;

    /* baseObject */
    const char *base = base_dn ? base_dn : "";
    asn1_value_t *base_obj = asn1_create_octet_string((const uint8_t *)base, strlen(base));
    if (!base_obj || asn1_sequence_add_child(search, base_obj) < 0) goto error;

    /* scope (ENUMERATED) */
    asn1_value_t *scope_val = asn1_create_integer(scope);
    if (!scope_val) goto error;
    scope_val->type = ASN1_TYPE_INTEGER;  /* ENUMERATED uses same encoding */
    scope_val->tag = 0x0a;  /* ENUMERATED tag */
    if (asn1_sequence_add_child(search, scope_val) < 0) goto error;

    /* derefAliases (ENUMERATED) */
    asn1_value_t *deref_val = asn1_create_integer(deref_aliases);
    if (!deref_val) goto error;
    deref_val->tag = 0x0a;
    if (asn1_sequence_add_child(search, deref_val) < 0) goto error;

    /* sizeLimit */
    asn1_value_t *size_val = asn1_create_integer(size_limit);
    if (!size_val || asn1_sequence_add_child(search, size_val) < 0) goto error;

    /* timeLimit */
    asn1_value_t *time_val = asn1_create_integer(time_limit);
    if (!time_val || asn1_sequence_add_child(search, time_val) < 0) goto error;

    /* typesOnly */
    asn1_value_t *types_val = asn1_create_boolean(types_only);
    if (!types_val || asn1_sequence_add_child(search, types_val) < 0) goto error;

    /* filter */
    const char *flt = filter_str ? filter_str : "(objectClass=*)";
    const char *flt_ptr = flt;
    asn1_value_t *filter = parse_filter(&flt_ptr);
    if (!filter || asn1_sequence_add_child(search, filter) < 0) {
        asn1_free(search);
        return LDAP_BUILD_ERROR_FILTER;
    }

    /* attributes (SEQUENCE OF) */
    asn1_value_t *attrs = asn1_create_sequence();
    if (!attrs || asn1_sequence_add_child(search, attrs) < 0) goto error;

    if (attributes) {
        for (const char **a = attributes; *a; a++) {
            asn1_value_t *attr = asn1_create_octet_string((const uint8_t *)*a, strlen(*a));
            if (!attr || asn1_sequence_add_child(attrs, attr) < 0) goto error;
        }
    }

    return wrap_ldap_message(message_id, search, out, out_len);

error:
    asn1_free(search);
    return LDAP_BUILD_ERROR_MEMORY;
}

/*
 * AddRequest ::= [APPLICATION 8] SEQUENCE {
 *     entry  LDAPDN,
 *     attributes AttributeList
 * }
 */
int ldap_build_add_request(
    int message_id,
    const char *dn,
    const ldap_attribute_t *attrs,
    size_t attr_count,
    uint8_t *out,
    size_t *out_len
) {
    if (!dn || !attrs || !out || !out_len) return LDAP_BUILD_ERROR_INVALID;

    asn1_value_t *add = create_app_sequence(LDAP_REQ_ADD);
    if (!add) return LDAP_BUILD_ERROR_MEMORY;

    /* entry DN */
    asn1_value_t *entry = asn1_create_octet_string((const uint8_t *)dn, strlen(dn));
    if (!entry || asn1_sequence_add_child(add, entry) < 0) goto error;

    /* attributes */
    asn1_value_t *attr_list = asn1_create_sequence();
    if (!attr_list || asn1_sequence_add_child(add, attr_list) < 0) goto error;

    for (size_t i = 0; i < attr_count; i++) {
        asn1_value_t *attr_seq = asn1_create_sequence();
        if (!attr_seq) goto error;

        /* type */
        asn1_value_t *type = asn1_create_octet_string(
            (const uint8_t *)attrs[i].type, strlen(attrs[i].type));
        if (!type || asn1_sequence_add_child(attr_seq, type) < 0) {
            asn1_free(attr_seq);
            goto error;
        }

        /* vals (SET OF) */
        asn1_value_t *vals = asn1_create_set();
        if (!vals || asn1_sequence_add_child(attr_seq, vals) < 0) {
            asn1_free(attr_seq);
            goto error;
        }

        for (size_t j = 0; j < attrs[i].value_count; j++) {
            asn1_value_t *val = asn1_create_octet_string(
                attrs[i].values[j].data, attrs[i].values[j].length);
            if (!val || asn1_set_add_child(vals, val) < 0) {
                asn1_free(attr_seq);
                goto error;
            }
        }

        if (asn1_sequence_add_child(attr_list, attr_seq) < 0) {
            asn1_free(attr_seq);
            goto error;
        }
    }

    return wrap_ldap_message(message_id, add, out, out_len);

error:
    asn1_free(add);
    return LDAP_BUILD_ERROR_MEMORY;
}

/*
 * DelRequest ::= [APPLICATION 10] LDAPDN
 */
int ldap_build_delete_request(
    int message_id,
    const char *dn,
    uint8_t *out,
    size_t *out_len
) {
    if (!dn || !out || !out_len) return LDAP_BUILD_ERROR_INVALID;

    asn1_value_t *del = asn1_create_octet_string((const uint8_t *)dn, strlen(dn));
    if (!del) return LDAP_BUILD_ERROR_MEMORY;

    del->tag_class = 1;  /* APPLICATION */
    del->tag_number = LDAP_REQ_DELETE;
    del->tag = 0x4a;  /* APPLICATION 10 primitive */

    return wrap_ldap_message(message_id, del, out, out_len);
}

/*
 * ModifyRequest ::= [APPLICATION 6] SEQUENCE {
 *     object  LDAPDN,
 *     changes SEQUENCE OF change
 * }
 * change ::= SEQUENCE {
 *     operation ENUMERATED { add(0), delete(1), replace(2) },
 *     modification PartialAttribute
 * }
 */
int ldap_build_modify_request(
    int message_id,
    const char *dn,
    const ldap_modification_t *mods,
    size_t mod_count,
    uint8_t *out,
    size_t *out_len
) {
    if (!dn || !mods || !out || !out_len) return LDAP_BUILD_ERROR_INVALID;

    asn1_value_t *modify = create_app_sequence(LDAP_REQ_MODIFY);
    if (!modify) return LDAP_BUILD_ERROR_MEMORY;

    /* object DN */
    asn1_value_t *obj = asn1_create_octet_string((const uint8_t *)dn, strlen(dn));
    if (!obj || asn1_sequence_add_child(modify, obj) < 0) goto error;

    /* changes */
    asn1_value_t *changes = asn1_create_sequence();
    if (!changes || asn1_sequence_add_child(modify, changes) < 0) goto error;

    for (size_t i = 0; i < mod_count; i++) {
        asn1_value_t *change = asn1_create_sequence();
        if (!change) goto error;

        /* operation (ENUMERATED) */
        asn1_value_t *op = asn1_create_integer(mods[i].operation);
        if (!op) { asn1_free(change); goto error; }
        op->tag = 0x0a;  /* ENUMERATED */
        if (asn1_sequence_add_child(change, op) < 0) { asn1_free(change); goto error; }

        /* modification (PartialAttribute) */
        asn1_value_t *mod_attr = asn1_create_sequence();
        if (!mod_attr) { asn1_free(change); goto error; }

        asn1_value_t *type = asn1_create_octet_string(
            (const uint8_t *)mods[i].attr.type, strlen(mods[i].attr.type));
        if (!type || asn1_sequence_add_child(mod_attr, type) < 0) {
            asn1_free(mod_attr);
            asn1_free(change);
            goto error;
        }

        asn1_value_t *vals = asn1_create_set();
        if (!vals || asn1_sequence_add_child(mod_attr, vals) < 0) {
            asn1_free(mod_attr);
            asn1_free(change);
            goto error;
        }

        for (size_t j = 0; j < mods[i].attr.value_count; j++) {
            asn1_value_t *val = asn1_create_octet_string(
                mods[i].attr.values[j].data, mods[i].attr.values[j].length);
            if (!val || asn1_set_add_child(vals, val) < 0) {
                asn1_free(mod_attr);
                asn1_free(change);
                goto error;
            }
        }

        if (asn1_sequence_add_child(change, mod_attr) < 0) {
            asn1_free(change);
            goto error;
        }

        if (asn1_sequence_add_child(changes, change) < 0) {
            asn1_free(change);
            goto error;
        }
    }

    return wrap_ldap_message(message_id, modify, out, out_len);

error:
    asn1_free(modify);
    return LDAP_BUILD_ERROR_MEMORY;
}

/*
 * ModifyDNRequest ::= [APPLICATION 12] SEQUENCE {
 *     entry        LDAPDN,
 *     newrdn       RelativeLDAPDN,
 *     deleteoldrdn BOOLEAN,
 *     newSuperior  [0] LDAPDN OPTIONAL
 * }
 */
int ldap_build_modifydn_request(
    int message_id,
    const char *dn,
    const char *new_rdn,
    int delete_old_rdn,
    const char *new_superior,
    uint8_t *out,
    size_t *out_len
) {
    if (!dn || !new_rdn || !out || !out_len) return LDAP_BUILD_ERROR_INVALID;

    asn1_value_t *moddn = create_app_sequence(LDAP_REQ_MODDN);
    if (!moddn) return LDAP_BUILD_ERROR_MEMORY;

    /* entry */
    asn1_value_t *entry = asn1_create_octet_string((const uint8_t *)dn, strlen(dn));
    if (!entry || asn1_sequence_add_child(moddn, entry) < 0) goto error;

    /* newrdn */
    asn1_value_t *rdn = asn1_create_octet_string((const uint8_t *)new_rdn, strlen(new_rdn));
    if (!rdn || asn1_sequence_add_child(moddn, rdn) < 0) goto error;

    /* deleteoldrdn */
    asn1_value_t *del_old = asn1_create_boolean(delete_old_rdn);
    if (!del_old || asn1_sequence_add_child(moddn, del_old) < 0) goto error;

    /* newSuperior (optional) */
    if (new_superior) {
        asn1_value_t *sup = create_ctx_primitive(0, (const uint8_t *)new_superior, strlen(new_superior));
        if (!sup || asn1_sequence_add_child(moddn, sup) < 0) goto error;
    }

    return wrap_ldap_message(message_id, moddn, out, out_len);

error:
    asn1_free(moddn);
    return LDAP_BUILD_ERROR_MEMORY;
}

/*
 * CompareRequest ::= [APPLICATION 14] SEQUENCE {
 *     entry LDAPDN,
 *     ava   AttributeValueAssertion
 * }
 */
int ldap_build_compare_request(
    int message_id,
    const char *dn,
    const char *attribute,
    const uint8_t *value,
    size_t value_len,
    uint8_t *out,
    size_t *out_len
) {
    if (!dn || !attribute || !value || !out || !out_len) return LDAP_BUILD_ERROR_INVALID;

    asn1_value_t *compare = create_app_sequence(LDAP_REQ_COMPARE);
    if (!compare) return LDAP_BUILD_ERROR_MEMORY;

    /* entry */
    asn1_value_t *entry = asn1_create_octet_string((const uint8_t *)dn, strlen(dn));
    if (!entry || asn1_sequence_add_child(compare, entry) < 0) goto error;

    /* ava */
    asn1_value_t *ava = create_ava(attribute, value, value_len);
    if (!ava || asn1_sequence_add_child(compare, ava) < 0) goto error;

    return wrap_ldap_message(message_id, compare, out, out_len);

error:
    asn1_free(compare);
    return LDAP_BUILD_ERROR_MEMORY;
}

/*
 * AbandonRequest ::= [APPLICATION 16] MessageID
 */
int ldap_build_abandon_request(
    int message_id,
    int abandon_id,
    uint8_t *out,
    size_t *out_len
) {
    if (!out || !out_len) return LDAP_BUILD_ERROR_INVALID;

    asn1_value_t *abandon = asn1_create_integer(abandon_id);
    if (!abandon) return LDAP_BUILD_ERROR_MEMORY;

    abandon->tag_class = 1;  /* APPLICATION */
    abandon->tag_number = LDAP_REQ_ABANDON;
    abandon->tag = 0x50;  /* APPLICATION 16 primitive */

    return wrap_ldap_message(message_id, abandon, out, out_len);
}
