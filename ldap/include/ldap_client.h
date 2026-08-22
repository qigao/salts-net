#ifndef LDAP_CLIENT_H
#define LDAP_CLIENT_H


#include "ldap_api.h"
#include "platform.h"
#include "ldap_types.h"
#include "ldap_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ldap_client_s ldap_client_t;

typedef struct {
    const char *url; // e.g., "ldap://localhost:389"
    int timeout_ms;  // Operation timeout
} ldap_client_config_t;

// Lifecycle
TURBONET_LDAP_C_API ldap_client_t *ldap_client_create(const ldap_client_config_t *config);
TURBONET_LDAP_C_API void ldap_client_destroy(ldap_client_t *client);
TURBONET_LDAP_C_API int ldap_client_connect(ldap_client_t *client); // Explicit connect or auto-connect

// Operations
// Synchronous APIs for simplicity, matching SNMP client style

// Simple Bind
TURBONET_LDAP_C_API int ldap_client_simple_bind(ldap_client_t *client, const char *dn, const char *password, ldap_result_data_t *result);

// Unbind
TURBONET_LDAP_C_API int ldap_client_unbind(ldap_client_t *client);

// Search
// Callback for search entries
typedef void (*ldap_search_cb)(ldap_client_t *client, const ldap_entry_t *entry, void *user_data);

typedef struct {
    const char *base_dn;
    int scope; // LDAP_SCOPE_*
    const char *filter;
    const char **attrs; // NULL for all user attributes
    bool types_only;
    int size_limit;
    int time_limit;
} ldap_search_params_t;

TURBONET_LDAP_C_API int ldap_client_search(ldap_client_t *client, const ldap_search_params_t *params, ldap_search_cb callback, void *user_data, ldap_result_data_t *result);

// Add
TURBONET_LDAP_C_API int ldap_client_add(ldap_client_t *client, const char *dn, const ldap_attribute_t *attrs, size_t attr_count, ldap_result_data_t *result);

// Delete
TURBONET_LDAP_C_API int ldap_client_delete(ldap_client_t *client, const char *dn, ldap_result_data_t *result);

// Modify
TURBONET_LDAP_C_API int ldap_client_modify(ldap_client_t *client, const char *dn, const ldap_modification_t *mods, size_t mod_count, ldap_result_data_t *result);

// Modify DN
TURBONET_LDAP_C_API int ldap_client_rename(ldap_client_t *client, const char *dn, const char *new_rdn, const char *new_parent, int delete_old_rdn, ldap_result_data_t *result);

// Compare
TURBONET_LDAP_C_API int ldap_client_compare(ldap_client_t *client, const char *dn, const char *attr, const char *value, size_t value_len, ldap_result_data_t *result);

// Utilities
TURBONET_LDAP_C_API const char *ldap_err2string(int err);
TURBONET_LDAP_C_API void ldap_result_free(ldap_result_data_t *result); // Frees strings inside result

#ifdef __cplusplus
}
#endif

#endif // LDAP_CLIENT_H
