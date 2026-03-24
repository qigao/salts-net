/**
 * @file tlv_schema_parser.h
 * @brief TLV schema parser internal API
 */

#ifndef TLV_SCHEMA_PARSER_H
#define TLV_SCHEMA_PARSER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Field types */
typedef enum {
    TLV_TYPE_INT32,
    TLV_TYPE_INT64,
    TLV_TYPE_DOUBLE,
    TLV_TYPE_STRING,
    TLV_TYPE_BYTES,
    TLV_TYPE_MESSAGE,
} TlvFieldType;

/* Tag marker bits carried in the encoded tag. Payload bytes are still
 * length-prefixed for every field in the current TLV format. */
typedef enum {
    WIRE_VARINT = 0,
    WIRE_FIXED64 = 1,
    WIRE_LENGTH_DELIMITED = 2,
    WIRE_FIXED32 = 5,
} TlvWireType;

/* Field definition */
typedef struct TlvField {
    int field_number;
    TlvFieldType type;
    TlvWireType wire_type;
    char name[64];
    char type_name[64];
} TlvField;

/* Message definition */
typedef struct TlvMessage {
    char name[64];
    TlvField* fields;
    int field_count;
    int field_capacity;
    int field_map[256];  /* Fast lookup: field_number -> fields index, -1 if absent */
} TlvMessage;

/* Schema */
typedef struct TlvSchema {
    TlvMessage* messages;
    int message_count;
    int message_capacity;
    char error[256];
} TlvSchema;

/* Public API */
TlvSchema* tlv_schema_parse_file(const char* path);
void tlv_schema_free(TlvSchema* schema);
TlvMessage* tlv_schema_find_message(TlvSchema* schema, const char* name);

#ifdef __cplusplus
}
#endif

#endif /* TLV_SCHEMA_PARSER_H */
