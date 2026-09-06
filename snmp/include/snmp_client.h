/**
 * @file snmp_client.h
 * @brief SNMP Client with UDP Transport (Synchronous API)
 */

#ifndef SNMP_CLIENT_H
#define SNMP_CLIENT_H


#include "snmp_api.h"
#include "platform.h"
#include "snmp_types.h"
#include "snmp_parser.h"
#include "snmp_builder.h"
#include "snmp_usm.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct snmp_client_s snmp_client_t;

/* Client configuration */
typedef struct {
    const char *host;           /* Target host (IP or hostname) */
    uint16_t port;              /* Target port (default: 161) */
    const char *community;      /* SNMP community string (default: "public") */
    snmp_version_t version;     /* SNMP version (default: SNMP_VERSION_2C) */

    /* Timeout and retry */
    uint32_t timeout_ms;        /* Timeout in milliseconds (default: 5000) */
    uint32_t retries;           /* Number of retries (default: 3) */

    /* Buffers */
    size_t recv_buffer_size;    /* Receive buffer size (default: 8192) */

    /* SNMPv3 USM security (only for version = SNMP_VERSION_3) */
    const char *security_name;           /* User name */
    const char *auth_password;           /* Authentication password */
    snmp_auth_protocol_t auth_protocol;  /* Authentication protocol */
    const char *priv_password;           /* Privacy password */
    snmp_priv_protocol_t priv_protocol;  /* Privacy protocol */
    snmp_security_level_t security_level; /* Security level */
} snmp_client_config_t;

/* Error codes */
#define SNMP_CLIENT_OK              0
#define SNMP_CLIENT_ERROR_INVALID  -1   /* Invalid parameters */
#define SNMP_CLIENT_ERROR_MEMORY   -2   /* Memory allocation failed */
#define SNMP_CLIENT_ERROR_NETWORK  -3   /* Network error */
#define SNMP_CLIENT_ERROR_TIMEOUT  -4   /* Request timeout */
#define SNMP_CLIENT_ERROR_RESPONSE -5   /* Invalid response */
#define SNMP_CLIENT_ERROR_SNMP     -6   /* SNMP error (check error_status) */

/**
 * Create SNMP client
 *
 * @param config Client configuration (if NULL, uses defaults)
 * @return Client handle, or NULL on error
 *
 * Example:
 *   snmp_client_config_t config = {
 *       .host = "192.168.1.1",
 *       .port = 161,
 *       .community = "public",
 *       .version = SNMP_VERSION_2C,
 *       .timeout_ms = 5000,
 *       .retries = 3
 *   };
 *   snmp_client_t *client = snmp_client_create(&config);
 */
SALTSNET_SNMP_C_API snmp_client_t *snmp_client_create(const snmp_client_config_t *config);

/**
 * Destroy SNMP client
 */
SALTSNET_SNMP_C_API void snmp_client_destroy(snmp_client_t *client);

/**
 * Send SNMP GetRequest and wait for response
 *
 * @param client SNMP client
 * @param oids Array of OIDs to query
 * @param oid_count Number of OIDs
 * @param response Output response message (allocated by caller)
 * @return SNMP_CLIENT_OK on success, negative error code on failure
 *
 * Example:
 *   snmp_oid_t oid;
 *   snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid);
 *
 *   snmp_message_t response;
 *   int result = snmp_client_get(client, &oid, 1, &response);
 *
 *   if (result == SNMP_CLIENT_OK) {
 *       printf("sysDescr: %.*s\n",
 *              response.pdu.varbinds[0].value.bytes.len,
 *              response.pdu.varbinds[0].value.bytes.data);
 *   }
 */
SALTSNET_SNMP_C_API int snmp_client_get(
    snmp_client_t *client,
    const snmp_oid_t *oids,
    size_t oid_count,
    snmp_message_t *response
);

/**
 * Send SNMP GetNextRequest and wait for response
 *
 * @param client SNMP client
 * @param oids Array of OIDs (starting points for walk)
 * @param oid_count Number of OIDs
 * @param response Output response message
 * @return SNMP_CLIENT_OK on success, negative error code on failure
 */
SALTSNET_SNMP_C_API int snmp_client_get_next(
    snmp_client_t *client,
    const snmp_oid_t *oids,
    size_t oid_count,
    snmp_message_t *response
);

/**
 * Send SNMP SetRequest and wait for response
 *
 * @param client SNMP client
 * @param varbinds Array of variable bindings (OID + value)
 * @param varbind_count Number of varbinds
 * @param response Output response message
 * @return SNMP_CLIENT_OK on success, negative error code on failure
 */
SALTSNET_SNMP_C_API int snmp_client_set(
    snmp_client_t *client,
    const snmp_varbind_t *varbinds,
    size_t varbind_count,
    snmp_message_t *response
);

/**
 * Perform SNMP Walk (retrieve all OIDs under a subtree)
 *
 * @param client SNMP client
 * @param root_oid Root OID to walk
 * @param callback Callback for each OID/value pair
 * @param user_data User data passed to callback
 * @return Number of OIDs retrieved, or negative error code
 *
 * Example:
 *   void print_oid(const snmp_oid_t *oid, const snmp_varbind_t *varbind, void *userdata) {
 *       char oid_str[256];
 *       snmp_oid_to_string(oid, oid_str, sizeof(oid_str));
 *       printf("%s = %d\n", oid_str, varbind->value.i32);
 *   }
 *
 *   snmp_oid_t root;
 *   snmp_oid_from_string("1.3.6.1.2.1.2.2.1", &root);  // ifTable
 *   int count = snmp_client_walk(client, &root, print_oid, NULL);
 */
typedef void (*snmp_walk_cb)(
    const snmp_oid_t *oid,
    const snmp_varbind_t *varbind,
    void *user_data
);

SALTSNET_SNMP_C_API int snmp_client_walk(
    snmp_client_t *client,
    const snmp_oid_t *root_oid,
    snmp_walk_cb callback,
    void *user_data
);

/**
 * Get last error details
 */
SALTSNET_SNMP_C_API const char *snmp_client_get_error(snmp_client_t *client);

/**
 * Set timeout (in milliseconds)
 */
SALTSNET_SNMP_C_API void snmp_client_set_timeout(snmp_client_t *client, uint32_t timeout_ms);

/**
 * Set retry count
 */
SALTSNET_SNMP_C_API void snmp_client_set_retries(snmp_client_t *client, uint32_t retries);

#ifdef __cplusplus
}
#endif

#endif /* SNMP_CLIENT_H */
