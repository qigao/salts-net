#ifndef SNMP_MIB_H
#define SNMP_MIB_H

#include "snmp_types.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

// MIB OID entry
typedef struct {
    const char *name;
    snmp_oid_t oid;
    const char *description;
    snmp_type_t type;
    int access;  // 0=not-accessible, 1=read-only, 2=read-write, 3=write-only
} snmp_oid_entry_t;

// MIB module
typedef struct {
    const char *name;
    const snmp_oid_entry_t *oid_table;
    size_t oid_count;
} snmp_mib_module_t;

// MIB registry
typedef struct {
    snmp_mib_module_t **modules;
    size_t module_count;
    size_t module_capacity;
} snmp_mib_registry_t;

// MIB registry functions
CXX_C_API snmp_mib_registry_t *snmp_mib_registry_create(void);
CXX_C_API void snmp_mib_registry_destroy(snmp_mib_registry_t *registry);
CXX_C_API int snmp_mib_registry_add_module(snmp_mib_registry_t *registry, 
                                           const snmp_mib_module_t *module);

// OID lookup functions
CXX_C_API const snmp_oid_entry_t *snmp_mib_lookup_oid(const snmp_mib_registry_t *registry,
                                                      const snmp_oid_t *oid);
CXX_C_API const snmp_oid_entry_t *snmp_mib_lookup_name(const snmp_mib_registry_t *registry,
                                                       const char *name);

// OID navigation
CXX_C_API const snmp_oid_entry_t *snmp_mib_get_next_oid(const snmp_mib_registry_t *registry,
                                                        const snmp_oid_t *oid);

// Utility functions
CXX_C_API char *snmp_oid_to_string(const snmp_oid_t *oid);
CXX_C_API int snmp_string_to_oid(const char *str, snmp_oid_t *oid);
CXX_C_API int snmp_oid_compare(const snmp_oid_t *oid1, const snmp_oid_t *oid2);
CXX_C_API int snmp_oid_is_prefix(const snmp_oid_t *prefix, const snmp_oid_t *oid);

#ifdef __cplusplus
}
#endif

#endif // SNMP_MIB_H