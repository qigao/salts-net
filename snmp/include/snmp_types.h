/**
 * @file snmp_types.h
 * @brief SNMP Protocol Data Structures (SNMPv1/v2c/v3)
 */

#ifndef SNMP_TYPES_H
#define SNMP_TYPES_H

#include "platform.h"
#include <cmeta/meta.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ============================================================================
 * SNMP Version
 * ============================================================================ */

Enum(snmp_version_t,
    (SNMP_VERSION_1, 0, "v1"),
    (SNMP_VERSION_2C, 1, "v2c"),
    (SNMP_VERSION_3, 3, "v3")
);

/* ============================================================================
 * SNMPv3 Security (RFC 3414 - USM)
 * ============================================================================ */

/* Security levels */
Enum(snmp_security_level_t,
    (SNMP_SEC_LEVEL_NOAUTH_NOPRIV, 0, "no_auth_no_priv"),
    (SNMP_SEC_LEVEL_AUTH_NOPRIV, 1, "auth_no_priv"),
    (SNMP_SEC_LEVEL_AUTH_PRIV, 3, "auth_priv")
);

/* Authentication protocols */
Enum(snmp_auth_protocol_t,
    (SNMP_AUTH_NONE, 0, "none"),
    (SNMP_AUTH_MD5, 1, "md5"),
    (SNMP_AUTH_SHA1, 2, "sha1")
);

/* Privacy (encryption) protocols */
Enum(snmp_priv_protocol_t,
    (SNMP_PRIV_NONE, 0, "none"),
    (SNMP_PRIV_DES, 1, "des"),
    (SNMP_PRIV_AES128, 2, "aes128")
);

/* USM Security Parameters */
typedef struct {
    uint8_t *authoritative_engine_id;   /* Authoritative engine ID */
    size_t engine_id_len;
    uint32_t engine_boots;              /* Engine boots (for time sync) */
    uint32_t engine_time;               /* Engine time (seconds since boot) */
    char *user_name;                    /* Security user name */
    uint8_t auth_params[12];            /* Authentication parameters (HMAC) */
    uint8_t priv_params[8];             /* Privacy parameters (salt/IV) */
} snmp_usm_params_t;

/* SNMPv3 User Credentials */
typedef struct {
    char *user_name;                    /* User name */
    snmp_auth_protocol_t auth_protocol; /* Authentication protocol */
    snmp_priv_protocol_t priv_protocol; /* Privacy protocol */

    /* Authentication key (localized) */
    uint8_t auth_key[32];               /* Capacity; MD5/SHA-1 use 16/20 bytes */
    size_t auth_key_len;

    /* Privacy key (localized) */
    uint8_t priv_key[32];               /* Capacity; DES/AES-128 require 16 bytes */
    size_t priv_key_len;
} snmp_v3_user_t;

/* SNMPv3 Message Header */
typedef struct {
    uint32_t msg_id;                    /* Message ID */
    uint32_t msg_max_size;              /* Max message size */
    uint8_t msg_flags;                  /* Flags (auth/priv/reportable) */
    uint32_t msg_security_model;        /* Security model (3 = USM) */
} snmp_v3_header_t;

/* Message flags bits */
#define SNMP_MSG_FLAG_AUTH       0x01   /* Authenticated */
#define SNMP_MSG_FLAG_PRIV       0x02   /* Encrypted */
#define SNMP_MSG_FLAG_REPORTABLE 0x04   /* Expects report */

/* ============================================================================
 * SNMP PDU Types
 * ============================================================================ */

typedef enum {
    SNMP_PDU_GET_REQUEST      = 0xA0,  /* Context [0] - Get single/multiple OIDs */
    SNMP_PDU_GET_NEXT_REQUEST = 0xA1,  /* Context [1] - Get next OID (MIB walk) */
    SNMP_PDU_GET_RESPONSE     = 0xA2,  /* Context [2] - Agent response */
    SNMP_PDU_SET_REQUEST      = 0xA3,  /* Context [3] - Set OID value */
    SNMP_PDU_TRAP             = 0xA4,  /* Context [4] - SNMPv1 trap (async notification) */
    SNMP_PDU_GET_BULK_REQUEST = 0xA5,  /* Context [5] - SNMPv2c bulk retrieval */
    SNMP_PDU_INFORM_REQUEST   = 0xA6,  /* Context [6] - SNMPv2c acknowledged notification */
    SNMP_PDU_TRAP_V2          = 0xA7,  /* Context [7] - SNMPv2c trap */
    SNMP_PDU_REPORT           = 0xA8,  /* Context [8] - SNMPv3 report */
} snmp_pdu_type_t;

/* ============================================================================
 * SNMP Error Status (RFC 1905)
 * ============================================================================ */

typedef enum {
    SNMP_ERROR_NOERROR         = 0,   /* No error */
    SNMP_ERROR_TOOBIG          = 1,   /* Response too large */
    SNMP_ERROR_NOSUCHNAME      = 2,   /* OID not found (v1 only) */
    SNMP_ERROR_BADVALUE        = 3,   /* Invalid value */
    SNMP_ERROR_READONLY        = 4,   /* OID is read-only */
    SNMP_ERROR_GENERR          = 5,   /* General error */
    SNMP_ERROR_NOACCESS        = 6,   /* Access denied (v2c) */
    SNMP_ERROR_WRONGTYPE       = 7,   /* Wrong type */
    SNMP_ERROR_WRONGLENGTH     = 8,   /* Wrong length */
    SNMP_ERROR_WRONGENCODING   = 9,   /* Wrong encoding */
    SNMP_ERROR_WRONGVALUE      = 10,  /* Wrong value */
    SNMP_ERROR_NOCREATION      = 11,  /* Cannot create */
    SNMP_ERROR_INCONSISTENTVALUE = 12, /* Inconsistent value */
    SNMP_ERROR_RESOURCEUNAVAILABLE = 13, /* Resource unavailable */
    SNMP_ERROR_COMMITFAILED    = 14,  /* Commit failed */
    SNMP_ERROR_UNDOFAILED      = 15,  /* Undo failed */
    SNMP_ERROR_AUTHORIZATIONERROR = 16, /* Authorization error */
    SNMP_ERROR_NOTWRITABLE     = 17,  /* Not writable */
    SNMP_ERROR_INCONSISTENTNAME = 18, /* Inconsistent name */
} snmp_error_status_t;

/* ============================================================================
 * SNMP Value Types (Application Tags)
 * ============================================================================ */

typedef enum {
    /* Universal ASN.1 types (handled by asn1_der.h) */
    SNMP_TYPE_INTEGER      = 0x02,  /* INTEGER */
    SNMP_TYPE_OCTET_STRING = 0x04,  /* OCTET STRING */
    SNMP_TYPE_NULL         = 0x05,  /* NULL */
    SNMP_TYPE_OID          = 0x06,  /* OBJECT IDENTIFIER */

    /* SNMP Application types (RFC 1155) */
    SNMP_TYPE_IPADDRESS    = 0x40,  /* Application [0] - IpAddress (4 octets) */
    SNMP_TYPE_COUNTER32    = 0x41,  /* Application [1] - Counter32 (monotonic) */
    SNMP_TYPE_GAUGE32      = 0x42,  /* Application [2] - Gauge32 (non-negative) */
    SNMP_TYPE_TIMETICKS    = 0x43,  /* Application [3] - TimeTicks (hundredths of second) */
    SNMP_TYPE_OPAQUE       = 0x44,  /* Application [4] - Opaque (arbitrary encoding) */
    SNMP_TYPE_COUNTER64    = 0x46,  /* Application [6] - Counter64 (SNMPv2c) */

    /* Exception values (SNMPv2c - RFC 1905) */
    SNMP_TYPE_NOSUCHOBJECT   = 0x80,  /* Context [0] - No such object */
    SNMP_TYPE_NOSUCHINSTANCE = 0x81,  /* Context [1] - No such instance */
    SNMP_TYPE_ENDOFMIBVIEW   = 0x82,  /* Context [2] - End of MIB view */
} snmp_value_type_t;

/* ============================================================================
 * SNMP Data Structures
 * ============================================================================ */

/* Forward declaration */
struct asn1_value;
typedef struct snmp_pdu_s snmp_pdu_t;

/**
 * SNMP OID (Object Identifier)
 * Example: "1.3.6.1.2.1.1.1.0" (sysDescr.0)
 */
typedef struct {
    uint32_t *components;   /* OID components array */
    size_t count;           /* Number of components */
} snmp_oid_t;

/**
 * SNMP Variable Binding (name-value pair)
 */
typedef struct {
    snmp_oid_t oid;                /* Variable name (OID) */
    snmp_value_type_t value_type;  /* Value type tag */
    union {
        int32_t i32;               /* INTEGER, Counter32, Gauge32, TimeTicks */
        int64_t i64;               /* Counter64 */
        struct {
            uint8_t *data;
            size_t len;
        } bytes;                   /* OCTET STRING, IpAddress, Opaque */
        snmp_oid_t oid;            /* OBJECT IDENTIFIER */
    } value;
} snmp_varbind_t;

/**
 * SNMP PDU (Protocol Data Unit)
 */
typedef struct snmp_pdu_s {
    snmp_pdu_type_t type;          /* PDU type */
    int32_t request_id;            /* Request identifier */
    int32_t error_status;          /* Error status (0 = no error) */
    int32_t error_index;           /* Index of variable in error (1-based) */

    /* For GetBulkRequest only */
    int32_t non_repeaters;         /* Number of non-repeating variables */
    int32_t max_repetitions;       /* Maximum repetitions */

    /* Variable bindings */
    snmp_varbind_t *varbinds;      /* Array of variable bindings */
    size_t varbind_count;          /* Number of variable bindings */
} snmp_pdu_t;

/**
 * SNMPv3 Scoped PDU
 */
typedef struct {
    uint8_t *context_engine_id;         /* Context engine ID */
    size_t context_engine_id_len;
    char *context_name;                 /* Context name */
    size_t context_name_len;
    snmp_pdu_t pdu;                     /* The actual PDU */
} snmp_scoped_pdu_t;

/**
 * SNMP Message (complete packet)
 */
typedef struct {
    snmp_version_t version;        /* SNMP version */

    /* SNMPv1/v2c community string */
    char *community;               /* Community string (e.g., "public", "private") */
    size_t community_len;

    /* SNMPv3 security */
    snmp_v3_header_t v3_header;    /* Message header (version 3 only) */
    snmp_usm_params_t usm_params;  /* USM security parameters (version 3 only) */
    snmp_scoped_pdu_t scoped_pdu;  /* Scoped PDU (version 3 only) */

    /* PDU (for v1/v2c, this is used directly; for v3, it's in scoped_pdu.pdu) */
    snmp_pdu_t pdu;
} snmp_message_t;

/* ============================================================================
 * Common OID Definitions (RFC 1213 - MIB-II)
 * ============================================================================ */

/* System Group (1.3.6.1.2.1.1) */
#define SNMP_OID_SYSDESCR      "1.3.6.1.2.1.1.1.0"    /* System description */
#define SNMP_OID_SYSOBJECTID   "1.3.6.1.2.1.1.2.0"    /* System object ID */
#define SNMP_OID_SYSUPTIME     "1.3.6.1.2.1.1.3.0"    /* System uptime */
#define SNMP_OID_SYSCONTACT    "1.3.6.1.2.1.1.4.0"    /* System contact */
#define SNMP_OID_SYSNAME       "1.3.6.1.2.1.1.5.0"    /* System name */
#define SNMP_OID_SYSLOCATION   "1.3.6.1.2.1.1.6.0"    /* System location */

/* Interfaces Group (1.3.6.1.2.1.2) */
#define SNMP_OID_IFNUMBER      "1.3.6.1.2.1.2.1.0"    /* Number of interfaces */
#define SNMP_OID_IFTABLE       "1.3.6.1.2.1.2.2"      /* Interface table */
#define SNMP_OID_IFDESCR       "1.3.6.1.2.1.2.2.1.2"  /* Interface description */
#define SNMP_OID_IFTYPE        "1.3.6.1.2.1.2.2.1.3"  /* Interface type */
#define SNMP_OID_IFSPEED       "1.3.6.1.2.1.2.2.1.5"  /* Interface speed */
#define SNMP_OID_IFOPERSTATUS  "1.3.6.1.2.1.2.2.1.8"  /* Interface status */

/* IP Group (1.3.6.1.2.1.4) */
#define SNMP_OID_IPINRECEIVES  "1.3.6.1.2.1.4.3.0"    /* IP packets received */
#define SNMP_OID_IPFORWDATAGRAMS "1.3.6.1.2.1.4.6.0"  /* IP packets forwarded */

/* TCP Group (1.3.6.1.2.1.6) */
#define SNMP_OID_TCPCONNTABLE  "1.3.6.1.2.1.6.13"     /* TCP connection table */

/* UDP Group (1.3.6.1.2.1.7) */
#define SNMP_OID_UDPINDATAGRAMS "1.3.6.1.2.1.7.1.0"   /* UDP datagrams received */

#endif /* SNMP_TYPES_H */
