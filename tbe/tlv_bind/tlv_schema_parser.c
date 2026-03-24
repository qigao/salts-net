/**
 * @file tlv_schema_parser.c
 * @brief Parse .tlvschema files into internal representation
 */

#include "tlv_schema_parser.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/**
 * @brief Create empty schema
 */
TlvSchema *tlv_schema_create(void) {
  TlvSchema *schema = calloc(1, sizeof(TlvSchema));
  schema->message_capacity = 8;
  schema->messages = calloc(schema->message_capacity, sizeof(TlvMessage));
  return schema;
}

/**
 * @brief Free schema
 */
void tlv_schema_free(TlvSchema *schema) {
  if (!schema) return;
  for (int i = 0; i < schema->message_count; i++) {
    free(schema->messages[i].fields);
  }
  free(schema->messages);
  free(schema);
}

/**
 * @brief Parse type string to TlvFieldType
 */
static int parse_field_type(const char *type_str, TlvFieldType *out_type, TlvWireType *out_wire) {
  if (strcmp(type_str, "int32") == 0) {
    *out_type = TLV_TYPE_INT32;
    *out_wire = WIRE_VARINT;
    return 1;
  }
  if (strcmp(type_str, "int64") == 0) {
    *out_type = TLV_TYPE_INT64;
    *out_wire = WIRE_VARINT;
    return 1;
  }
  if (strcmp(type_str, "double") == 0) {
    *out_type = TLV_TYPE_DOUBLE;
    *out_wire = WIRE_FIXED64;
    return 1;
  }
  if (strcmp(type_str, "string") == 0) {
    *out_type = TLV_TYPE_STRING;
    *out_wire = WIRE_LENGTH_DELIMITED;
    return 1;
  }
  if (strcmp(type_str, "bytes") == 0) {
    *out_type = TLV_TYPE_BYTES;
    *out_wire = WIRE_LENGTH_DELIMITED;
    return 1;
  }
  /* Assume it's a nested message type */
  *out_type = TLV_TYPE_MESSAGE;
  *out_wire = WIRE_LENGTH_DELIMITED;
  return 1;
}

/**
 * @brief Add field to message
 */
static int add_field(TlvMessage *msg, int field_num, const char *type_str, const char *name) {
  if (msg->field_count >= msg->field_capacity) {
    int new_capacity = msg->field_capacity * 2;
    TlvField *new_fields = realloc(msg->fields, (size_t)new_capacity * sizeof(TlvField));
    if (!new_fields) {
      return 0;
    }
    msg->field_capacity = new_capacity;
    msg->fields = new_fields;
  }

  TlvField *field = &msg->fields[msg->field_count++];
  field->field_number = field_num;
  parse_field_type(type_str, &field->type, &field->wire_type);
  strncpy(field->name, name, sizeof(field->name) - 1);
  if (field->type == TLV_TYPE_MESSAGE) {
    strncpy(field->type_name, type_str, sizeof(field->type_name) - 1);
  }

  /* Build fast lookup map */
  if (field_num < 256) {
    msg->field_map[field_num] = msg->field_count - 1;
  }
  return 1;
}

/**
 * @brief Parse schema file
 *
 * Format:
 *   message MessageName {
 *       1: int32 field_name
 *       2: string another_field
 *   }
 */
TlvSchema *tlv_schema_parse_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    return NULL;
  }

  TlvSchema *schema = tlv_schema_create();
  char line[256];
  TlvMessage *current_msg = NULL;

  while (fgets(line, sizeof(line), f)) {
    /* Skip comments and empty lines */
    char *p = line;
    while (isspace(*p))
      p++;
    if (*p == '#' || *p == '\0') continue;

    /* Parse "message Name {" */
    if (strncmp(p, "message", 7) == 0) {
      p += 7;
      while (isspace(*p))
        p++;

      /* Add new message */
      if (schema->message_count >= schema->message_capacity) {
        int new_capacity = schema->message_capacity * 2;
        TlvMessage *new_messages =
            realloc(schema->messages, (size_t)new_capacity * sizeof(TlvMessage));
        if (!new_messages) {
          fclose(f);
          tlv_schema_free(schema);
          return NULL;
        }
        schema->message_capacity = new_capacity;
        schema->messages = new_messages;
      }

      current_msg = &schema->messages[schema->message_count++];
      memset(current_msg, 0, sizeof(TlvMessage));
      current_msg->field_capacity = 8;
      current_msg->fields = calloc(current_msg->field_capacity, sizeof(TlvField));
      if (!current_msg->fields) {
        fclose(f);
        tlv_schema_free(schema);
        return NULL;
      }
      for (size_t i = 0; i < sizeof(current_msg->field_map) / sizeof(current_msg->field_map[0]);
           ++i) {
        current_msg->field_map[i] = -1;
      }

      /* Extract message name */
      char *name_end = p;
      while (*name_end && !isspace(*name_end) && *name_end != '{')
        name_end++;
      size_t name_len = name_end - p;
      if (name_len >= sizeof(current_msg->name)) name_len = sizeof(current_msg->name) - 1;
      memcpy(current_msg->name, p, name_len);
      current_msg->name[name_len] = '\0';
      continue;
    }

    /* Parse "}" */
    if (*p == '}') {
      current_msg = NULL;
      continue;
    }

    /* Parse field: "int32 field_name" (field number auto-assigned) */
    if (current_msg && !isspace(*p) && *p != '}') {
      /* Auto-assign field number based on declaration order */
      int field_num = current_msg->field_count + 1;

      /* Extract type */
      char type_str[64] = {0};
      char *type_end = p;
      while (*type_end && !isspace(*type_end))
        type_end++;
      size_t type_len = type_end - p;
      if (type_len >= sizeof(type_str)) type_len = sizeof(type_str) - 1;
      memcpy(type_str, p, type_len);
      p = type_end;
      while (isspace(*p))
        p++;

      /* Extract field name */
      char field_name[64] = {0};
      char *name_end = p;
      while (*name_end && !isspace(*name_end) && *name_end != '#')
        name_end++;
      size_t name_len = name_end - p;
      if (name_len >= sizeof(field_name)) name_len = sizeof(field_name) - 1;
      memcpy(field_name, p, name_len);

      if (!add_field(current_msg, field_num, type_str, field_name)) {
        fclose(f);
        tlv_schema_free(schema);
        return NULL;
      }
    }
  }

  fclose(f);
  return schema;
}

/**
 * @brief Find message by name
 */
TlvMessage *tlv_schema_find_message(TlvSchema *schema, const char *name) {
  for (int i = 0; i < schema->message_count; i++) {
    if (strcmp(schema->messages[i].name, name) == 0) {
      return &schema->messages[i];
    }
  }
  return NULL;
}
