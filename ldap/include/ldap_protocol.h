#ifndef LDAP_PROTOCOL_H
#define LDAP_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

// LDAP v3 Protocol OpCodes (Application Tags)
#define LDAP_REQ_BIND           0
#define LDAP_RES_BIND           1
#define LDAP_REQ_UNBIND         2
#define LDAP_REQ_SEARCH         3
#define LDAP_RES_SEARCH_ENTRY   4
#define LDAP_RES_SEARCH_DONE    5
#define LDAP_RES_SEARCH_REF     19
#define LDAP_REQ_MODIFY         6
#define LDAP_RES_MODIFY         7
#define LDAP_REQ_ADD            8
#define LDAP_RES_ADD            9
#define LDAP_REQ_DELETE         10
#define LDAP_RES_DELETE         11
#define LDAP_REQ_MODDN          12
#define LDAP_RES_MODDN          13
#define LDAP_REQ_COMPARE        14
#define LDAP_RES_COMPARE        15
#define LDAP_REQ_ABANDON        16
#define LDAP_REQ_EXTENDED       23
#define LDAP_RES_EXTENDED       24

// LDAP Result Codes
#define LDAP_SUCCESS                      0
#define LDAP_OPERATIONS_ERROR             1
#define LDAP_PROTOCOL_ERROR               2
#define LDAP_TIMELIMIT_EXCEEDED           3
#define LDAP_SIZELIMIT_EXCEEDED           4
#define LDAP_COMPARE_FALSE                5
#define LDAP_COMPARE_TRUE                 6
#define LDAP_AUTH_METHOD_NOT_SUPPORTED    7
#define LDAP_STRONG_AUTH_REQUIRED         8
#define LDAP_REFERRAL                     10
#define LDAP_ADMIN_LIMIT_EXCEEDED         11
#define LDAP_UNAVAILABLE_CRITICAL_EXTENSION 12
#define LDAP_CONFIDENTIALITY_REQUIRED     13
#define LDAP_SASL_BIND_IN_PROGRESS        14
#define LDAP_NO_SUCH_ATTRIBUTE            16
#define LDAP_UNDEFINED_TYPE               17
#define LDAP_INAPPROPRIATE_MATCHING       18
#define LDAP_CONSTRAINT_VIOLATION         19
#define LDAP_TYPE_OR_VALUE_EXISTS         20
#define LDAP_INVALID_ATTRIBUTE_SYNTAX     21
#define LDAP_NO_SUCH_OBJECT               32
#define LDAP_ALIAS_PROBLEM                33
#define LDAP_INVALID_DN_SYNTAX            34
#define LDAP_IS_LEAF                      35
#define LDAP_ALIAS_DEREF_PROBLEM          36
#define LDAP_INAPPROPRIATE_AUTH           48
#define LDAP_INVALID_CREDENTIALS          49
#define LDAP_INSUFFICIENT_ACCESS          50
#define LDAP_BUSY                         51
#define LDAP_UNAVAILABLE                  52
#define LDAP_UNWILLING_TO_PERFORM         53
#define LDAP_LOOP_DETECT                  54
#define LDAP_NAMING_VIOLATION             64
#define LDAP_OBJECT_CLASS_VIOLATION       65
#define LDAP_NOT_ALLOWED_ON_NONLEAF       66
#define LDAP_NOT_ALLOWED_ON_RDN           67
#define LDAP_ENTRY_ALREADY_EXISTS         68
#define LDAP_OBJECT_CLASS_MODS_PROHIBITED 69
#define LDAP_AFFECTS_MULTIPLE_DSAS        71
#define LDAP_OTHER                        80

// Authentication Methods (Simplification)
#define LDAP_AUTH_SIMPLE  0x80 // Context specific 0, primitive

// Search Scope
#define LDAP_SCOPE_BASE           0
#define LDAP_SCOPE_ONELEVEL       1
#define LDAP_SCOPE_SUBTREE        2

// Filter Types (Context Specific Tags)
#define LDAP_FILTER_AND             0xa0
#define LDAP_FILTER_OR              0xa1
#define LDAP_FILTER_NOT             0xa2
#define LDAP_FILTER_EQUALITY        0xa3
#define LDAP_FILTER_SUBSTRINGS      0xa4
#define LDAP_FILTER_GE              0xa5
#define LDAP_FILTER_LE              0xa6
#define LDAP_FILTER_PRESENT         0xa7
#define LDAP_FILTER_APPROX          0xa8
#define LDAP_FILTER_EXT             0xa9

#ifdef __cplusplus
}
#endif

#endif // LDAP_PROTOCOL_H
