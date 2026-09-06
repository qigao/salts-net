#ifndef LDAP_PARSER_H
#define LDAP_PARSER_H


#include "ldap_api.h"
#include "platform.h"
#include "ldap_types.h"
#include "ldap_protocol.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parse error codes
#define LDAP_PARSE_OK              0
#define LDAP_PARSE_ERROR_MEMORY   -1
#define LDAP_PARSE_ERROR_INVALID  -2
#define LDAP_PARSE_ERROR_TRUNCATED -3
#define LDAP_PARSE_ERROR_FORMAT   -4

// Parse result - holds parsed message and consumed bytes
typedef struct {
    ldap_message_t *message;
    size_t bytes_consumed;
} ldap_parse_result_t;

// Parse a single LDAP message from buffer
// Returns: LDAP_PARSE_OK on success, error code on failure
// On success, result->message contains parsed message, result->bytes_consumed indicates how many bytes were used
SALTSNET_LDAP_C_API int ldap_parse_message(
    const uint8_t *data,
    size_t len,
    ldap_parse_result_t *result
);

// Check if buffer contains a complete LDAP message
// Returns: >0 = complete message length, 0 = incomplete, <0 = error
SALTSNET_LDAP_C_API int ldap_message_complete(
    const uint8_t *data,
    size_t len
);

// Free parsed message
SALTSNET_LDAP_C_API void ldap_message_free(ldap_message_t *msg);

// Free search entry
SALTSNET_LDAP_C_API void ldap_entry_free(ldap_entry_t *entry);

// Free result data
SALTSNET_LDAP_C_API void ldap_result_free(ldap_result_data_t *result);

// Utility: get result code name
SALTSNET_LDAP_C_API const char *ldap_result_code_str(int code);

#ifdef __cplusplus
}
#endif

#endif // LDAP_PARSER_H
