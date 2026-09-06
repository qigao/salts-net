/**
 * @file snmp_builder.h
 * @brief SNMP Message Builder (constructs SNMP requests)
 */

#ifndef SNMP_BUILDER_H
#define SNMP_BUILDER_H


#include "snmp_api.h"
#include "platform.h"
#include "snmp_types.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define SNMP_BUILD_OK               0
#define SNMP_BUILD_ERROR_INVALID   -1   /* Invalid input */
#define SNMP_BUILD_ERROR_MEMORY    -2   /* Memory allocation failed */
#define SNMP_BUILD_ERROR_BUFFER    -3   /* Output buffer too small */

/**
 * Build SNMP GetRequest message
 *
 * @param version SNMP version (SNMP_VERSION_1 or SNMP_VERSION_2C)
 * @param community Community string (e.g., "public")
 * @param request_id Request ID (should be unique)
 * @param oids Array of OIDs to query
 * @param oid_count Number of OIDs
 * @param out Output buffer
 * @param out_len [in] Buffer size, [out] Bytes written
 * @return SNMP_BUILD_OK on success, negative error code on failure
 *
 * Example:
 *   snmp_oid_t oids[2];
 *   snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oids[0]);  // sysDescr
 *   snmp_oid_from_string("1.3.6.1.2.1.1.5.0", &oids[1]);  // sysName
 *   uint8_t packet[512];
 *   size_t packet_len = sizeof(packet);
 *   int result = snmp_build_get_request(
 *       SNMP_VERSION_2C, "public", 1234, oids, 2, packet, &packet_len
 *   );
 */
SALTSNET_SNMP_C_API int snmp_build_get_request(
    snmp_version_t version,
    const char *community,
    int32_t request_id,
    const snmp_oid_t *oids,
    size_t oid_count,
    uint8_t *out,
    size_t *out_len
);

/**
 * Build SNMP GetNextRequest message (for MIB walking)
 */
SALTSNET_SNMP_C_API int snmp_build_get_next_request(
    snmp_version_t version,
    const char *community,
    int32_t request_id,
    const snmp_oid_t *oids,
    size_t oid_count,
    uint8_t *out,
    size_t *out_len
);

/**
 * Build SNMP SetRequest message
 */
SALTSNET_SNMP_C_API int snmp_build_set_request(
    snmp_version_t version,
    const char *community,
    int32_t request_id,
    const snmp_varbind_t *varbinds,
    size_t varbind_count,
    uint8_t *out,
    size_t *out_len
);

/**
 * Build SNMP GetBulkRequest message (SNMPv2c only)
 *
 * @param non_repeaters Number of non-repeating variables
 * @param max_repetitions Maximum repetitions for repeating variables
 */
SALTSNET_SNMP_C_API int snmp_build_get_bulk_request(
    const char *community,
    int32_t request_id,
    int32_t non_repeaters,
    int32_t max_repetitions,
    const snmp_oid_t *oids,
    size_t oid_count,
    uint8_t *out,
    size_t *out_len
);

/**
 * Helper: Parse OID string to snmp_oid_t
 *
 * @param oid_str OID string (e.g., "1.3.6.1.2.1.1.1.0")
 * @param oid Output OID structure
 * @return 0 on success, -1 on error
 *
 * NOTE: Caller must free oid->components when done
 */
SALTSNET_SNMP_C_API int snmp_oid_from_string(const char *oid_str, snmp_oid_t *oid);

/**
 * Helper: Convert OID to string
 */
SALTSNET_SNMP_C_API int snmp_oid_to_string(const snmp_oid_t *oid, char *buf, size_t buf_len);

/**
 * Helper: Compare two OIDs
 * @return 0 if equal, <0 if oid1 < oid2, >0 if oid1 > oid2
 */
SALTSNET_SNMP_C_API int snmp_oid_compare(const snmp_oid_t *oid1, const snmp_oid_t *oid2);

/**
 * Helper: Free OID components
 */
SALTSNET_SNMP_C_API void snmp_oid_free(snmp_oid_t *oid);

/**
 * Build SNMPv3 GetRequest message with USM security
 *
 * @param request_id Request ID
 * @param oids Array of OIDs to query
 * @param oid_count Number of OIDs
 * @param usm_params USM security parameters (engineID, boots, time, user, auth, priv)
 * @param user User credentials (for authentication/encryption)
 * @param security_level Security level (noAuthNoPriv/authNoPriv/authPriv)
 * @param out Output buffer
 * @param out_len [in] Buffer size, [out] Encoded length
 * @return SNMP_BUILD_OK on success, negative error code on failure
 */
SALTSNET_SNMP_C_API int snmp_build_v3_get_request(
    int32_t request_id,
    const snmp_oid_t *oids,
    size_t oid_count,
    const snmp_usm_params_t *usm_params,
    const snmp_v3_user_t *user,
    snmp_security_level_t security_level,
    uint8_t *out,
    size_t *out_len
);

/** Build an SNMPv3 GetNextRequest using the same USM ownership contract as
 * snmp_build_v3_get_request(). */
SALTSNET_SNMP_C_API int snmp_build_v3_get_next_request(
    int32_t request_id,
    const snmp_oid_t *oids,
    size_t oid_count,
    const snmp_usm_params_t *usm_params,
    const snmp_v3_user_t *user,
    snmp_security_level_t security_level,
    uint8_t *out,
    size_t *out_len
);

/** Build an SNMPv3 SetRequest with typed variable bindings. */
SALTSNET_SNMP_C_API int snmp_build_v3_set_request(
    int32_t request_id,
    const snmp_varbind_t *varbinds,
    size_t varbind_count,
    const snmp_usm_params_t *usm_params,
    const snmp_v3_user_t *user,
    snmp_security_level_t security_level,
    uint8_t *out,
    size_t *out_len
);

#ifdef __cplusplus
}
#endif

#endif /* SNMP_BUILDER_H */
