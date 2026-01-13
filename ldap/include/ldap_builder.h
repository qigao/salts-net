#ifndef LDAP_BUILDER_H
#define LDAP_BUILDER_H

#include "platform.h"
#include "ldap_types.h"
#include "ldap_protocol.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build error codes
#define LDAP_BUILD_OK            0
#define LDAP_BUILD_ERROR_MEMORY  -1
#define LDAP_BUILD_ERROR_INVALID -2
#define LDAP_BUILD_ERROR_BUFFER  -3
#define LDAP_BUILD_ERROR_FILTER  -4

// BindRequest (Simple Authentication)
CXX_C_API int ldap_build_bind_request(
    int message_id,
    int version,              // Usually 3
    const char *dn,           // Distinguished Name (can be empty for anonymous)
    const char *password,     // Password (can be NULL for anonymous)
    uint8_t *out,
    size_t *out_len
);

// UnbindRequest
CXX_C_API int ldap_build_unbind_request(
    int message_id,
    uint8_t *out,
    size_t *out_len
);

// SearchRequest
CXX_C_API int ldap_build_search_request(
    int message_id,
    const char *base_dn,
    int scope,                // LDAP_SCOPE_BASE, LDAP_SCOPE_ONELEVEL, LDAP_SCOPE_SUBTREE
    int deref_aliases,        // 0-3
    int size_limit,
    int time_limit,
    int types_only,
    const char *filter,       // LDAP filter string e.g. "(cn=admin)"
    const char **attributes,  // NULL-terminated array, or NULL for all
    uint8_t *out,
    size_t *out_len
);

// AddRequest
CXX_C_API int ldap_build_add_request(
    int message_id,
    const char *dn,
    const ldap_attribute_t *attrs,
    size_t attr_count,
    uint8_t *out,
    size_t *out_len
);

// DeleteRequest
CXX_C_API int ldap_build_delete_request(
    int message_id,
    const char *dn,
    uint8_t *out,
    size_t *out_len
);

// ModifyRequest
CXX_C_API int ldap_build_modify_request(
    int message_id,
    const char *dn,
    const ldap_modification_t *mods,
    size_t mod_count,
    uint8_t *out,
    size_t *out_len
);

// ModifyDNRequest (Rename)
CXX_C_API int ldap_build_modifydn_request(
    int message_id,
    const char *dn,
    const char *new_rdn,
    int delete_old_rdn,
    const char *new_superior,  // Can be NULL
    uint8_t *out,
    size_t *out_len
);

// CompareRequest
CXX_C_API int ldap_build_compare_request(
    int message_id,
    const char *dn,
    const char *attribute,
    const uint8_t *value,
    size_t value_len,
    uint8_t *out,
    size_t *out_len
);

// AbandonRequest
CXX_C_API int ldap_build_abandon_request(
    int message_id,
    int abandon_id,
    uint8_t *out,
    size_t *out_len
);

#ifdef __cplusplus
}
#endif

#endif // LDAP_BUILDER_H
