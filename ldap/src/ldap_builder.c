/** @file ldap_builder.c @brief LDAP BER message builder. */
#include "ldap_builder.h"
#include <salts/asn1_value.h>
#include <stdlib.h>
#include <string.h>

static salts_asn1_value *parse_filter(const char **text);

/* Consumes child on both success and failure. */
static int add_child(salts_asn1_value *parent, salts_asn1_value *child) {
    if (!child) return -1;
    if (salts_asn1_value_add_child(parent, child) != SALTS_ASN1_OK) {
        salts_asn1_value_free(child);
        return -1;
    }
    return 0;
}

static int add_integer(salts_asn1_value *parent, int64_t number) {
    salts_asn1_value *value = NULL;
    if (salts_asn1_value_create_integer(number, &value) != SALTS_ASN1_OK)
        return -1;
    return add_child(parent, value);
}

static int add_octets(salts_asn1_value *parent, const void *data, size_t size) {
    salts_asn1_value *value = NULL;
    if (salts_asn1_value_create_octet_string(data, size, &value) != SALTS_ASN1_OK)
        return -1;
    return add_child(parent, value);
}

static int add_boolean(salts_asn1_value *parent, int boolean_value) {
    salts_asn1_value *value = NULL;
    if (salts_asn1_value_create_boolean(boolean_value, &value) != SALTS_ASN1_OK)
        return -1;
    return add_child(parent, value);
}

static salts_asn1_value *create_tagged_integer(uint8_t tag_class,
                                                uint32_t tag_number,
                                                int64_t number) {
    salts_asn1_value *integer = NULL, *tagged = NULL;
    const uint8_t *content = NULL;
    size_t content_size = 0u;
    if (salts_asn1_value_create_integer(number, &integer) != SALTS_ASN1_OK ||
        salts_asn1_value_get_content(integer, &content, &content_size) !=
            SALTS_ASN1_OK ||
        salts_asn1_value_create(tag_class, 0u, tag_number, content, content_size,
                                &tagged) != SALTS_ASN1_OK)
        tagged = NULL;
    salts_asn1_value_free(integer);
    return tagged;
}

static salts_asn1_value *create_app_sequence(uint32_t tag_number) {
    salts_asn1_value *value = NULL;
    if (salts_asn1_value_create(1u, 1u, tag_number, NULL, 0u, &value) !=
        SALTS_ASN1_OK)
        return NULL;
    return value;
}

static salts_asn1_value *create_ctx_primitive(uint32_t tag_number,
                                               const void *data, size_t size) {
    salts_asn1_value *value = NULL;
    if (salts_asn1_value_create(2u, 0u, tag_number, data, size, &value) !=
        SALTS_ASN1_OK)
        return NULL;
    return value;
}

static salts_asn1_value *create_ctx_constructed(uint32_t tag_number) {
    salts_asn1_value *value = NULL;
    if (salts_asn1_value_create(2u, 1u, tag_number, NULL, 0u, &value) !=
        SALTS_ASN1_OK)
        return NULL;
    return value;
}

static int wrap_ldap_message(int message_id, salts_asn1_value *operation,
                             uint8_t *out, size_t *out_len) {
    salts_asn1_value *message = NULL;
    size_t written = 0u;
    salts_asn1_status status;
    if (salts_asn1_value_create_sequence(&message) != SALTS_ASN1_OK) {
        salts_asn1_value_free(operation);
        return LDAP_BUILD_ERROR_MEMORY;
    }
    if (add_integer(message, message_id) != 0 ||
        add_child(message, operation) != 0) {
        salts_asn1_value_free(message);
        return LDAP_BUILD_ERROR_MEMORY;
    }
    status = salts_asn1_value_encode(message, SALTS_ASN1_BER, 0u, out,
                                     *out_len, &written);
    salts_asn1_value_free(message);
    *out_len = written;
    return status == SALTS_ASN1_OK ? LDAP_BUILD_OK : LDAP_BUILD_ERROR_BUFFER;
}

int ldap_build_bind_request(int message_id, int version, const char *dn,
                            const char *password, uint8_t *out,
                            size_t *out_len) {
    salts_asn1_value *bind;
    const char *name = dn ? dn : "";
    const char *secret = password ? password : "";
    if (!out || !out_len) return LDAP_BUILD_ERROR_INVALID;
    bind = create_app_sequence(LDAP_REQ_BIND);
    if (!bind) return LDAP_BUILD_ERROR_MEMORY;
    if (add_integer(bind, version) != 0 ||
        add_octets(bind, name, strlen(name)) != 0 ||
        add_child(bind, create_ctx_primitive(0u, secret, strlen(secret))) != 0) {
        salts_asn1_value_free(bind);
        return LDAP_BUILD_ERROR_MEMORY;
    }
    return wrap_ldap_message(message_id, bind, out, out_len);
}

int ldap_build_unbind_request(int message_id, uint8_t *out, size_t *out_len) {
    salts_asn1_value *unbind = NULL;
    if (!out || !out_len) return LDAP_BUILD_ERROR_INVALID;
    if (salts_asn1_value_create(1u, 0u, LDAP_REQ_UNBIND, NULL, 0u, &unbind) !=
        SALTS_ASN1_OK)
        return LDAP_BUILD_ERROR_MEMORY;
    return wrap_ldap_message(message_id, unbind, out, out_len);
}

static void skip_spaces(const char **text) {
    while (**text == ' ' || **text == '\t') ++*text;
}

static char *parse_attr_desc(const char **text) {
    const char *start = *text;
    size_t size;
    char *attribute;
    while (**text && **text != '=' && **text != '>' && **text != '<' &&
           **text != '~' && **text != ')' && **text != '(' && **text != '*')
        ++*text;
    size = (size_t)(*text - start);
    attribute = (char *)malloc(size + 1u);
    if (attribute) {
        memcpy(attribute, start, size);
        attribute[size] = '\0';
    }
    return attribute;
}

static uint8_t *parse_value(const char **text, size_t *out_size) {
    const char *start = *text;
    size_t size;
    uint8_t *value;
    while (**text && **text != ')' && **text != '*') ++*text;
    size = (size_t)(*text - start);
    value = (uint8_t *)malloc(size + 1u);
    if (value) {
        memcpy(value, start, size);
        value[size] = 0u;
        *out_size = size;
    }
    return value;
}

static salts_asn1_value *create_ava(const char *attribute,
                                     const uint8_t *value, size_t value_size) {
    salts_asn1_value *ava = NULL;
    if (salts_asn1_value_create_sequence(&ava) != SALTS_ASN1_OK) return NULL;
    if (add_octets(ava, attribute, strlen(attribute)) != 0 ||
        add_octets(ava, value, value_size) != 0) {
        salts_asn1_value_free(ava);
        return NULL;
    }
    return ava;
}

static salts_asn1_value *parse_filter_item(const char **text) {
    char *attribute = parse_attr_desc(text);
    uint32_t filter_type;
    uint8_t *value;
    size_t value_size = 0u;
    salts_asn1_value *ava = NULL, *tagged = NULL;
    if (!attribute) return NULL;
    if (**text == '=' && *(*text + 1) != '*') {
        ++*text;
        filter_type = 3u;
    } else if (**text == '>' && *(*text + 1) == '=') {
        *text += 2;
        filter_type = 5u;
    } else if (**text == '<' && *(*text + 1) == '=') {
        *text += 2;
        filter_type = 6u;
    } else if (**text == '~' && *(*text + 1) == '=') {
        *text += 2;
        filter_type = 8u;
    } else if (**text == '=' && *(*text + 1) == '*') {
        *text += 2;
        tagged = create_ctx_primitive(7u, attribute, strlen(attribute));
        free(attribute);
        return tagged;
    } else {
        free(attribute);
        return NULL;
    }
    value = parse_value(text, &value_size);
    if (value) ava = create_ava(attribute, value, value_size);
    free(attribute);
    free(value);
    if (!ava ||
        salts_asn1_value_create(2u, 1u, filter_type, NULL, 0u, &tagged) !=
            SALTS_ASN1_OK ||
        salts_asn1_value_child_count(ava) != 2u) {
        salts_asn1_value_free(ava);
        salts_asn1_value_free(tagged);
        return NULL;
    }
    /* LDAP filter tags replace the SEQUENCE tag but retain its two children. */
    for (size_t i = 0u; i < 2u; ++i) {
        const salts_asn1_value *source = salts_asn1_value_child_at(ava, i);
        const uint8_t *content = NULL;
        size_t size = 0u;
        salts_asn1_value *copy = NULL;
        if (salts_asn1_value_get_content(source, &content, &size) !=
                SALTS_ASN1_OK ||
            salts_asn1_value_create_octet_string(content, size, &copy) !=
                SALTS_ASN1_OK ||
            add_child(tagged, copy) != 0) {
            salts_asn1_value_free(tagged);
            salts_asn1_value_free(ava);
            return NULL;
        }
    }
    salts_asn1_value_free(ava);
    return tagged;
}

static salts_asn1_value *parse_filter(const char **text) {
    salts_asn1_value *filter = NULL;
    skip_spaces(text);
    if (**text != '(') return NULL;
    ++*text;
    skip_spaces(text);
    if (**text == '&' || **text == '|') {
        uint32_t tag = **text == '&' ? 0u : 1u;
        ++*text;
        filter = create_ctx_constructed(tag);
        if (!filter) return NULL;
        while (**text == '(') {
            salts_asn1_value *nested = parse_filter(text);
            if (add_child(filter, nested) != 0) {
                salts_asn1_value_free(filter);
                return NULL;
            }
            skip_spaces(text);
        }
    } else if (**text == '!') {
        ++*text;
        filter = create_ctx_constructed(2u);
        if (!filter || add_child(filter, parse_filter(text)) != 0) {
            salts_asn1_value_free(filter);
            return NULL;
        }
    } else {
        filter = parse_filter_item(text);
    }
    skip_spaces(text);
    if (**text == ')') ++*text;
    return filter;
}

int ldap_build_search_request(int message_id, const char *base_dn, int scope,
                              int deref_aliases, int size_limit,
                              int time_limit, int types_only,
                              const char *filter_str,
                              const char **attributes, uint8_t *out,
                              size_t *out_len) {
    salts_asn1_value *search = NULL, *value = NULL, *attributes_value = NULL;
    const char *base = base_dn ? base_dn : "";
    const char *filter_text = filter_str ? filter_str : "(objectClass=*)";
    if (!out || !out_len) return LDAP_BUILD_ERROR_INVALID;
    search = create_app_sequence(LDAP_REQ_SEARCH);
    if (!search || add_octets(search, base, strlen(base)) != 0) goto memory;
    value = create_tagged_integer(0u, 10u, scope);
    if (add_child(search, value) != 0) goto memory;
    value = create_tagged_integer(0u, 10u, deref_aliases);
    if (add_child(search, value) != 0 || add_integer(search, size_limit) != 0 ||
        add_integer(search, time_limit) != 0 ||
        add_boolean(search, types_only) != 0)
        goto memory;
    value = parse_filter(&filter_text);
    if (add_child(search, value) != 0) {
        salts_asn1_value_free(search);
        return LDAP_BUILD_ERROR_FILTER;
    }
    if (salts_asn1_value_create_sequence(&attributes_value) != SALTS_ASN1_OK ||
        add_child(search, attributes_value) != 0)
        goto memory;
    if (attributes) {
        for (const char **attribute = attributes; *attribute; ++attribute) {
            if (add_octets(attributes_value, *attribute, strlen(*attribute)) != 0)
                goto memory;
        }
    }
    return wrap_ldap_message(message_id, search, out, out_len);
memory:
    salts_asn1_value_free(search);
    return LDAP_BUILD_ERROR_MEMORY;
}

int ldap_build_add_request(int message_id, const char *dn,
                           const ldap_attribute_t *attributes,
                           size_t attribute_count, uint8_t *out,
                           size_t *out_len) {
    salts_asn1_value *request = NULL, *list = NULL;
    if (!dn || !attributes || !out || !out_len)
        return LDAP_BUILD_ERROR_INVALID;
    request = create_app_sequence(LDAP_REQ_ADD);
    if (!request || add_octets(request, dn, strlen(dn)) != 0 ||
        salts_asn1_value_create_sequence(&list) != SALTS_ASN1_OK ||
        add_child(request, list) != 0)
        goto error;
    for (size_t i = 0u; i < attribute_count; ++i) {
        salts_asn1_value *attribute = NULL, *values = NULL;
        if (!attributes[i].type ||
            salts_asn1_value_create_sequence(&attribute) != SALTS_ASN1_OK ||
            add_octets(attribute, attributes[i].type,
                       strlen(attributes[i].type)) != 0 ||
            salts_asn1_value_create_set(&values) != SALTS_ASN1_OK ||
            add_child(attribute, values) != 0) {
            salts_asn1_value_free(attribute);
            goto error;
        }
        for (size_t j = 0u; j < attributes[i].value_count; ++j)
            if (add_octets(values, attributes[i].values[j].data,
                           attributes[i].values[j].length) != 0) {
                salts_asn1_value_free(attribute);
                goto error;
            }
        if (add_child(list, attribute) != 0) goto error;
    }
    return wrap_ldap_message(message_id, request, out, out_len);
error:
    salts_asn1_value_free(request);
    return LDAP_BUILD_ERROR_MEMORY;
}

int ldap_build_delete_request(int message_id, const char *dn, uint8_t *out,
                              size_t *out_len) {
    salts_asn1_value *request = NULL;
    if (!dn || !out || !out_len) return LDAP_BUILD_ERROR_INVALID;
    if (salts_asn1_value_create(1u, 0u, LDAP_REQ_DELETE, dn, strlen(dn),
                                &request) != SALTS_ASN1_OK)
        return LDAP_BUILD_ERROR_MEMORY;
    return wrap_ldap_message(message_id, request, out, out_len);
}

int ldap_build_modify_request(int message_id, const char *dn,
                              const ldap_modification_t *modifications,
                              size_t modification_count, uint8_t *out,
                              size_t *out_len) {
    salts_asn1_value *request = NULL, *changes = NULL;
    if (!dn || !modifications || !out || !out_len)
        return LDAP_BUILD_ERROR_INVALID;
    request = create_app_sequence(LDAP_REQ_MODIFY);
    if (!request || add_octets(request, dn, strlen(dn)) != 0 ||
        salts_asn1_value_create_sequence(&changes) != SALTS_ASN1_OK ||
        add_child(request, changes) != 0)
        goto error;
    for (size_t i = 0u; i < modification_count; ++i) {
        salts_asn1_value *change = NULL, *attribute = NULL, *values = NULL;
        if (!modifications[i].attr.type ||
            salts_asn1_value_create_sequence(&change) != SALTS_ASN1_OK ||
            add_child(change, create_tagged_integer(
                                  0u, 10u, modifications[i].operation)) != 0 ||
            salts_asn1_value_create_sequence(&attribute) != SALTS_ASN1_OK ||
            add_octets(attribute, modifications[i].attr.type,
                       strlen(modifications[i].attr.type)) != 0 ||
            salts_asn1_value_create_set(&values) != SALTS_ASN1_OK ||
            add_child(attribute, values) != 0) {
            salts_asn1_value_free(change);
            salts_asn1_value_free(attribute);
            goto error;
        }
        for (size_t j = 0u; j < modifications[i].attr.value_count; ++j)
            if (add_octets(values, modifications[i].attr.values[j].data,
                           modifications[i].attr.values[j].length) != 0) {
                salts_asn1_value_free(change);
                salts_asn1_value_free(attribute);
                goto error;
            }
        if (add_child(change, attribute) != 0 || add_child(changes, change) != 0)
            goto error;
    }
    return wrap_ldap_message(message_id, request, out, out_len);
error:
    salts_asn1_value_free(request);
    return LDAP_BUILD_ERROR_MEMORY;
}

int ldap_build_modifydn_request(int message_id, const char *dn,
                                const char *new_rdn, int delete_old_rdn,
                                const char *new_superior, uint8_t *out,
                                size_t *out_len) {
    salts_asn1_value *request;
    if (!dn || !new_rdn || !out || !out_len)
        return LDAP_BUILD_ERROR_INVALID;
    request = create_app_sequence(LDAP_REQ_MODDN);
    if (!request || add_octets(request, dn, strlen(dn)) != 0 ||
        add_octets(request, new_rdn, strlen(new_rdn)) != 0 ||
        add_boolean(request, delete_old_rdn) != 0 ||
        (new_superior &&
         add_child(request, create_ctx_primitive(0u, new_superior,
                                                  strlen(new_superior))) != 0)) {
        salts_asn1_value_free(request);
        return LDAP_BUILD_ERROR_MEMORY;
    }
    return wrap_ldap_message(message_id, request, out, out_len);
}

int ldap_build_compare_request(int message_id, const char *dn,
                               const char *attribute, const uint8_t *value,
                               size_t value_size, uint8_t *out,
                               size_t *out_len) {
    salts_asn1_value *request;
    if (!dn || !attribute || !value || !out || !out_len)
        return LDAP_BUILD_ERROR_INVALID;
    request = create_app_sequence(LDAP_REQ_COMPARE);
    if (!request || add_octets(request, dn, strlen(dn)) != 0 ||
        add_child(request, create_ava(attribute, value, value_size)) != 0) {
        salts_asn1_value_free(request);
        return LDAP_BUILD_ERROR_MEMORY;
    }
    return wrap_ldap_message(message_id, request, out, out_len);
}

int ldap_build_abandon_request(int message_id, int abandon_id, uint8_t *out,
                               size_t *out_len) {
    salts_asn1_value *request;
    if (!out || !out_len) return LDAP_BUILD_ERROR_INVALID;
    request = create_tagged_integer(1u, LDAP_REQ_ABANDON, abandon_id);
    if (!request) return LDAP_BUILD_ERROR_MEMORY;
    return wrap_ldap_message(message_id, request, out, out_len);
}
