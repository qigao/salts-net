#ifndef LDAP_TYPES_H
#define LDAP_TYPES_H

#include "platform.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// LDAP Controls
typedef struct {
    char *oid;
    bool criticality;
    struct {
        uint8_t *data;
        size_t len;
    } value;
} ldap_control_t;

// LDAP Attribute Value
typedef struct {
    uint8_t *data;
    size_t length;
} ldap_value_t;

// LDAP Attribute / PartialAttribute
typedef struct {
    const char *type;           // Attribute Description (e.g. "cn")
    const ldap_value_t *values; // Array of values
    size_t value_count;
} ldap_attribute_t;

// LDAP Modification Operation
typedef enum {
    LDAP_MOD_ADD = 0,
    LDAP_MOD_DELETE = 1,
    LDAP_MOD_REPLACE = 2,
    LDAP_MOD_INCREMENT = 3 // RFC 4525
} ldap_mod_op_t;

// LDAP Modification
typedef struct {
    int operation;          // ldap_mod_op_t
    ldap_attribute_t attr;
} ldap_modification_t;

// LDAP Search Entry
typedef struct {
    char *dn;
    ldap_attribute_t *attributes;
    size_t attribute_count;
} ldap_entry_t;

// LDAP Result (Common for Bind, Add, Delete, Modify, etc.)
typedef struct {
    int result_code;
    char *matched_dn;
    char *diagnostic_message;
    char **referrals; // Array of referral URIs
    size_t referral_count;
} ldap_result_data_t;

// LDAP Message (PDU)
typedef struct ldap_message_s ldap_message_t;

struct ldap_message_s {
    int message_id;
    int protocol_op; // Application Tag (e.g. LDAP_RES_BIND)
    
    union {
        ldap_result_data_t bind_response;
        ldap_result_data_t generic_result; // Add, Del, Mod, Compare responses
        ldap_entry_t search_entry;
        ldap_result_data_t search_done;
        // ... Request types omitted for client receive side usually, but good to have if we build requests
    } payload;

    ldap_control_t *controls;
    size_t control_count;
    
    ldap_message_t *next; // For chaining results
};

#ifdef __cplusplus
}
#endif

#endif // LDAP_TYPES_H
