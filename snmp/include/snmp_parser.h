/**
 * @file snmp_parser.h
 * @brief SNMP Message Parser (uses ASN.1/BER decoder)
 */

#ifndef SNMP_PARSER_H
#define SNMP_PARSER_H


#include "snmp_api.h"
#include "platform.h"
#include "snmp_types.h"
#include "memory_pool.h"
#include <stdint.h>
#include <stddef.h>

/* Error codes */
#define SNMP_PARSE_OK                0
#define SNMP_PARSE_ERROR_INVALID    -1   /* Invalid input */
#define SNMP_PARSE_ERROR_MALFORMED  -2   /* Malformed SNMP message */
#define SNMP_PARSE_ERROR_MEMORY     -3   /* Memory allocation failed */
#define SNMP_PARSE_ERROR_VERSION    -4   /* Unsupported SNMP version */
#define SNMP_PARSE_ERROR_PDU_TYPE   -5   /* Unknown PDU type */
#define SNMP_PARSE_ERROR_VALUE_TYPE -6   /* Unknown value type */

/**
 * Parse SNMP message from wire format (BER-encoded)
 *
 * @param data Input BER-encoded SNMP message
 * @param len Length of input data
 * @param msg Output SNMP message structure
 * @param pool Memory pool for zero-allocation parsing (optional)
 * @return Number of bytes consumed on success, negative error code on failure
 *
 * Example:
 *   MemoryPool *pool = pool_create(8192);
 *   snmp_message_t msg;
 *   int result = snmp_parse(packet, packet_len, &msg, pool);
 *   if (result > 0) {
 *       // Access msg.pdu.varbinds[0].oid...
 *   }
 *   pool_destroy(pool);
 */
TURBONET_SNMP_C_API int snmp_parse(
    const uint8_t *data,
    size_t len,
    snmp_message_t *msg,
    MemoryPool *pool
);

/**
 * Free SNMP message (when not using memory pool)
 */
TURBONET_SNMP_C_API void snmp_message_free(snmp_message_t *msg);

/**
 * Parse and verify SNMPv3 message with USM security
 *
 * This function parses a v3 message, verifies authentication (if enabled),
 * and decrypts the scopedPDU (if privacy is enabled).
 *
 * @param data Input BER-encoded SNMPv3 message
 * @param len Length of input data
 * @param msg Output SNMP message structure
 * @param user User credentials for authentication/decryption
 * @param pool Memory pool for zero-allocation parsing (optional)
 * @return Number of bytes consumed on success, negative error code on failure
 */
TURBONET_SNMP_C_API int snmp_parse_v3(
    const uint8_t *data,
    size_t len,
    snmp_message_t *msg,
    const snmp_v3_user_t *user,
    MemoryPool *pool
);

#endif /* SNMP_PARSER_H */
