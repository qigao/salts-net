#ifndef TURBO_PARSER_H
#define TURBO_PARSER_H

#include "platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JSON Parser */
typedef struct json_value_s json_value_t;

typedef enum {
  TURBO_JSON_NULL,
  TURBO_JSON_BOOL,
  TURBO_JSON_NUMBER,
  TURBO_JSON_STRING,
  TURBO_JSON_ARRAY,
  TURBO_JSON_OBJECT
} turbo_json_type_t;

/**
 * @brief Parse JSON data.
 * @param data Input buffer.
 * @param len Buffer length.
 * @param out Address of a pointer (json_value_t **) to store the result.
 * @return 0 on success, error code otherwise.
 */
CXX_C_API int turbo_parse_json(const uint8_t *data, size_t len, void *out);

/**
 * @brief Free JSON data and set pointer to NULL.
 * @param out Address of the pointer (json_value_t **) to free.
 */
CXX_C_API void turbo_free_json(void *out);

/**
 * @brief Get the type of a JSON value.
 * @param value Pointer to the JSON value.
 * @return The type of the JSON value.
 */
CXX_C_API turbo_json_type_t turbo_json_type(const json_value_t *value);

/**
 * @brief Check if a JSON value is null.
 * @param value Pointer to the JSON value.
 * @return true if null, false otherwise.
 */
CXX_C_API bool turbo_json_is_null(const json_value_t *value);

/**
 * @brief Get boolean value from a JSON boolean node.
 * @param value Pointer to the JSON value.
 * @return The boolean value.
 */
CXX_C_API bool turbo_json_bool(const json_value_t *value);

/**
 * @brief Get numeric value from a JSON number node.
 * @param value Pointer to the JSON value.
 * @return The numeric value as a double.
 */
CXX_C_API double turbo_json_number(const json_value_t *value);

/**
 * @brief Get string value from a JSON string node.
 * @param value Pointer to the JSON value.
 * @return Pointer to the null-terminated string.
 */
CXX_C_API const char *turbo_json_string(const json_value_t *value);

/**
 * @brief Get the length of a JSON string.
 * @param value Pointer to the JSON value.
 * @return The length of the string in bytes.
 */
CXX_C_API size_t turbo_json_string_len(const json_value_t *value);

/**
 * @brief Get the number of properties in a JSON object.
 * @param obj Pointer to the JSON object.
 * @return Number of properties.
 */
CXX_C_API size_t turbo_json_object_size(const json_value_t *obj);

/**
 * @brief Get the key name of an object property by index.
 * @param obj Pointer to the JSON object.
 * @param index Index of the property.
 * @return Pointer to the key string.
 */
CXX_C_API const char *turbo_json_object_key(const json_value_t *obj, size_t index);

/**
 * @brief Get the value of an object property by index.
 * @param obj Pointer to the JSON object.
 * @param index Index of the property.
 * @return Pointer to the property value.
 */
CXX_C_API json_value_t *turbo_json_object_value(const json_value_t *obj, size_t index);

/**
 * @brief Get the value of an object property by key name.
 * @param obj Pointer to the JSON object.
 * @param key Key name to look up.
 * @return Pointer to the value if found, NULL otherwise.
 */
CXX_C_API json_value_t *turbo_json_object_get(const json_value_t *obj, const char *key);

/**
 * @brief Get the number of elements in a JSON array.
 * @param arr Pointer to the JSON array.
 * @return Number of elements.
 */
CXX_C_API size_t turbo_json_array_size(const json_value_t *arr);

/**
 * @brief Get an array element by index.
 * @param arr Pointer to the JSON array.
 * @param index Index of the element.
 * @return Pointer to the element value.
 */
CXX_C_API json_value_t *turbo_json_array_get(const json_value_t *arr, size_t index);

/**
 * @brief Convenience function to get an integer property from an object.
 * @param obj Pointer to the JSON object.
 * @param key Key name.
 * @param def Default value if key not found or not a number.
 * @return Integer value.
 */
CXX_C_API int turbo_json_get_int(const json_value_t *obj, const char *key, int def);

/**
 * @brief Convenience function to get a boolean property from an object.
 * @param obj Pointer to the JSON object.
 * @param key Key name.
 * @param def Default value if key not found or not a boolean.
 * @return Boolean value.
 */
CXX_C_API bool turbo_json_get_bool(const json_value_t *obj, const char *key, bool def);

/**
 * @brief Convenience function to get a double property from an object.
 * @param obj Pointer to the JSON object.
 * @param key Key name.
 * @param def Default value if key not found or not a number.
 * @return Double value.
 */
CXX_C_API double turbo_json_get_double(const json_value_t *obj, const char *key, double def);

/**
 * @brief Convenience function to get a string property from an object.
 * @param obj Pointer to the JSON object.
 * @param key Key name.
 * @return Pointer to string value if found, NULL otherwise.
 */
CXX_C_API const char *turbo_json_get_string(const json_value_t *obj, const char *key);

/**
 * @brief Serialize JSON structure to a string.
 * @param value Pointer to the JSON value to serialize.
 * @param out_len Optional pointer to store the output string length.
 * @return Pointer to the allocated string (must be freed with turbo_json_serialize_free).
 */
CXX_C_API char *turbo_json_serialize(const json_value_t *value, size_t *out_len);

/**
 * @brief Serialize a JSON value to a pretty-printed string with indentation.
 * @param value Pointer to the JSON value to serialize.
 * @param out_len Optional pointer to store the output string length.
 * @return Pointer to the allocated string (must be freed with turbo_json_serialize_free).
 */
CXX_C_API char *turbo_json_serialize_pretty(const json_value_t *value, size_t *out_len);

/**
 * @brief Serialize a JSON value to a pretty-printed string with CRLF line endings.
 * @param value Pointer to the JSON value to serialize.
 * @param out_len Optional pointer to store the output string length.
 * @return Pointer to the allocated string (must be freed with turbo_json_serialize_free).
 */
CXX_C_API char *turbo_json_serialize_pretty_crlf(const json_value_t *value, size_t *out_len);

/**
 * @brief Free a string allocated by turbo_json_serialize or turbo_json_serialize_pretty.
 * @param str Pointer to the serialized string.
 */
CXX_C_API void turbo_json_serialize_free(char *str);

/* JSON Builder/Modifier */
/**
 * @brief Create an empty JSON object.
 * @return Pointer to the newly created JSON object.
 */
CXX_C_API json_value_t *turbo_json_create_object(void);

/**
 * @brief Create an empty JSON array.
 * @return Pointer to the newly created JSON array.
 */
CXX_C_API json_value_t *turbo_json_create_array(void);

/**
 * @brief Create a JSON string node.
 * @param str Input null-terminated string.
 * @return Pointer to the newly created JSON string node.
 */
CXX_C_API json_value_t *turbo_json_create_string(const char *str);

/**
 * @brief Create a JSON number node.
 * @param num Numeric value.
 * @return Pointer to the newly created JSON number node.
 */
CXX_C_API json_value_t *turbo_json_create_number(double num);

/**
 * @brief Create a JSON boolean node.
 * @param val Boolean value.
 * @return Pointer to the newly created JSON boolean node.
 */
CXX_C_API json_value_t *turbo_json_create_bool(bool val);

/**
 * @brief Create a JSON null node.
 * @return Pointer to the newly created JSON null node.
 */
CXX_C_API json_value_t *turbo_json_create_null(void);

/**
 * @brief Add value to object (takes ownership of val).
 * @param obj Target object.
 * @param key Property key.
 * @param val Value node.
 */
CXX_C_API void turbo_json_object_add(json_value_t *obj, const char *key, json_value_t *val);

/**
 * @brief Add value to array (takes ownership of val).
 * @param arr Target array.
 * @param val Value node.
 */
CXX_C_API void turbo_json_array_add(json_value_t *arr, json_value_t *val);

/**
 * @brief Set/Update string property in object.
 * @param obj Target object.
 * @param key Property key.
 * @param val String value.
 */
CXX_C_API void turbo_json_object_set_string(json_value_t *obj, const char *key, const char *val);

/**
 * @brief Set/Update number property in object.
 * @param obj Target object.
 * @param key Property key.
 * @param val Numeric value.
 */
CXX_C_API void turbo_json_object_set_number(json_value_t *obj, const char *key, double val);

/**
 * @brief Set/Update boolean property in object.
 * @param obj Target object.
 * @param key Property key.
 * @param val Boolean value.
 */
CXX_C_API void turbo_json_object_set_bool(json_value_t *obj, const char *key, bool val);

/**
 * @brief Set property to null in object.
 * @param obj Target object.
 * @param key Property key.
 */
CXX_C_API void turbo_json_object_set_null(json_value_t *obj, const char *key);

/* XML Parser (cxml) */
typedef struct _cx_doc_node turbo_xml_doc_t;
typedef struct _cx_elem_node turbo_xml_node_t;

/**
 * @brief Parse XML data.
 * @param data Input buffer.
 * @param len Buffer length.
 * @param out Address of a pointer (turbo_xml_doc_t **) to store the result.
 * @return 0 on success, error code otherwise.
 */
CXX_C_API int turbo_parse_xml(const uint8_t *data, size_t len, void *out);

/**
 * @brief Free XML data and set pointer to NULL.
 * @param out Address of the pointer (turbo_xml_doc_t **) to free.
 */
CXX_C_API void turbo_free_xml(void *out);

/**
 * @brief Get the root element of an XML document.
 * @param doc Pointer to the XML document.
 * @return Pointer to the root element.
 */
CXX_C_API turbo_xml_node_t *turbo_xml_root_element(const turbo_xml_doc_t *doc);

/**
 * @brief Get the name of an XML node.
 * @param node Pointer to the XML node.
 * @return Pointer to the name string.
 */
CXX_C_API const char *turbo_xml_node_name(const turbo_xml_node_t *node);

/* CSV */
typedef struct csv_doc_s turbo_csv_doc_t;

/**
 * @brief Parse CSV data.
 * @param data Input buffer.
 * @param len Buffer length.
 * @param out Address of a pointer (turbo_csv_doc_t **) to store the result.
 * @return 0 on success, error code otherwise.
 */
CXX_C_API int turbo_parse_csv(const uint8_t *data, size_t len, void *out);

/**
 * @brief Free CSV data and set pointer to NULL.
 * @param out Address of the pointer (turbo_csv_doc_t **) to free.
 */
CXX_C_API void turbo_free_csv(void *out);

/**
 * @brief Get number of rows in CSV.
 * @param doc Pointer to CSV document.
 * @return Row count.
 */
CXX_C_API size_t turbo_csv_row_count(const turbo_csv_doc_t *doc);

/**
 * @brief Get number of columns in CSV.
 * @param doc Pointer to CSV document.
 * @return Column count.
 */
CXX_C_API size_t turbo_csv_column_count(const turbo_csv_doc_t *doc);

/**
 * @brief Get cell value as string.
 * @param doc Pointer to CSV document.
 * @param row Row index.
 * @param col Column index.
 * @return Cell string value.
 */
CXX_C_API const char *turbo_csv_get(const turbo_csv_doc_t *doc, size_t row, size_t col);

/**
 * @brief Get cell value as integer.
 * @param doc Pointer to CSV document.
 * @param row Row index.
 * @param col Column index.
 * @param def Default value.
 * @return cell integer value.
 */
CXX_C_API int turbo_csv_get_int(const turbo_csv_doc_t *doc, size_t row, size_t col, int def);

/**
 * @brief Get cell value as double.
 * @param doc Pointer to CSV document.
 * @param row Row index.
 * @param col Column index.
 * @param def Default value.
 * @return cell double value.
 */
CXX_C_API double turbo_csv_get_double(const turbo_csv_doc_t *doc, size_t row, size_t col,
                                      double def);

/**
 * @brief Get cell value as boolean.
 * @param doc Pointer to CSV document.
 * @param row Row index.
 * @param col Column index.
 * @param def Default value.
 * @return cell boolean value.
 */
CXX_C_API bool turbo_csv_get_bool(const turbo_csv_doc_t *doc, size_t row, size_t col, bool def);

/* INI */
typedef struct ini_s turbo_ini_t;

/**
 * @brief Parse INI data.
 * @param data Input buffer.
 * @param len Buffer length.
 * @param out Address of a pointer (turbo_ini_t **) to store the result.
 * @return 0 on success, error code otherwise.
 */
CXX_C_API int turbo_parse_ini(const uint8_t *data, size_t len, void *out);

/**
 * @brief Free INI data and set pointer to NULL.
 * @param out Address of the pointer (turbo_ini_t **) to free.
 */
CXX_C_API void turbo_free_ini(void *out);

/**
 * @brief Get string value from INI.
 * @param ini Pointer to INI document.
 * @param section Section name.
 * @param key Key name.
 * @return Value string if found, NULL otherwise.
 */
CXX_C_API const char *turbo_ini_get(const turbo_ini_t *ini, const char *section, const char *key);

/**
 * @brief Get integer value from INI.
 * @param ini Pointer to INI document.
 * @param section Section name.
 * @param key Key name.
 * @param def Default value.
 * @return Integer value.
 */
CXX_C_API int turbo_ini_get_int(const turbo_ini_t *ini, const char *section, const char *key,
                                int def);

/**
 * @brief Get boolean value from INI.
 * @param ini Pointer to INI document.
 * @param section Section name.
 * @param key Key name.
 * @param def Default value.
 * @return Boolean value.
 */
CXX_C_API bool turbo_ini_get_bool(const turbo_ini_t *ini, const char *section, const char *key,
                                  bool def);

/**
 * @brief Get double value from INI.
 * @param ini Pointer to INI document.
 * @param section Section name.
 * @param key Key name.
 * @param def Default value.
 * @return Double value.
 */
CXX_C_API double turbo_ini_get_double(const turbo_ini_t *ini, const char *section, const char *key,
                                      double def);

/* TLV */
typedef struct frame_s turbo_tlv_frame_t;

/**
 * @brief Parse TLV (Type-Length-Value) frame.
 * @param data Input buffer.
 * @param len Buffer length.
 * @param out Address of a pointer (turbo_tlv_frame_t **) to store the result.
 * @return 0 on success, error code otherwise.
 */
CXX_C_API int turbo_parse_tlv(const uint8_t *data, size_t len, void *out);

/**
 * @brief Free TLV data and set pointer to NULL.
 * @param out Address of the pointer (turbo_tlv_frame_t **) to free.
 */
CXX_C_API void turbo_free_tlv(void *out);

/**
 * @brief Get the message ID from a TLV frame.
 * @param frame Pointer to the TLV frame.
 * @return The message ID.
 */
CXX_C_API uint32_t turbo_tlv_msg_id(const turbo_tlv_frame_t *frame);

/**
 * @brief Get the protocol version from a TLV frame.
 * @param frame Pointer to the TLV frame.
 * @return Protocol version.
 */
CXX_C_API uint8_t turbo_tlv_version(const turbo_tlv_frame_t *frame);

/**
 * @brief Get the payload type from a TLV frame.
 * @param frame Pointer to the TLV frame.
 * @return Payload type.
 */
CXX_C_API uint8_t turbo_tlv_type(const turbo_tlv_frame_t *frame);

/**
 * @brief Get the payload size from a TLV frame.
 * @param frame Pointer to the TLV frame.
 * @return Payload size in bytes.
 */
CXX_C_API size_t turbo_tlv_payload_size(const turbo_tlv_frame_t *frame);

/**
 * @brief Get a pointer to the payload data in a TLV frame.
 * @param frame Pointer to the TLV frame.
 * @return Pointer to the payload data.
 */
CXX_C_API const char *turbo_tlv_payload(const turbo_tlv_frame_t *frame);

/**
 * @brief Get the CRC32 check value from a TLV frame.
 * @param frame Pointer to the TLV frame.
 * @return CRC32 value.
 */
CXX_C_API uint32_t turbo_tlv_crc32(const turbo_tlv_frame_t *frame);

/**
 * @brief Peek into a buffer to determine the total size of a TLV frame.
 * @param data Input buffer.
 * @param len Available buffer length.
 * @param out_size Pointer to store the detected total frame size.
 * @return 0 on success, error code if data is insufficient or invalid.
 */
CXX_C_API int turbo_tlv_peek_size(const uint8_t *data, size_t len, uint32_t *out_size);

/* URI Parser */
typedef struct uri_s uri_t;

typedef enum {
  TURBO_URI_HOST_UNKNOWN = 0,
  TURBO_URI_HOST_REGNAME,
  TURBO_URI_HOST_IPV6ADDR,
  TURBO_URI_HOST_IPV4ADDR,
  TURBO_URI_HOST_IPVFUTURE
} turbo_uri_host_type_t;

/**
 * @brief Parse a URI string.
 * @param data Input string data.
 * @param len String length.
 * @param out Address of a pointer (uri_t **) to store the result.
 * @return 0 on success, error code otherwise.
 */
CXX_C_API int turbo_parse_uri(const uint8_t *data, size_t len, void *out);

/**
 * @brief Free URI data and set pointer to NULL.
 * @param out Address of the pointer (uri_t **) to free.
 */
CXX_C_API void turbo_free_uri(void *out);

/**
 * @brief Get the scheme part of a URI (e.g., "http" or "ftp").
 * @param uri Pointer to the URI structure.
 * @return Pointer to the scheme string.
 */
CXX_C_API const char *turbo_uri_scheme(const uri_t *uri);

/**
 * @brief Get the user information part of a URI.
 * @param uri Pointer to the URI structure.
 * @return Pointer to the userinfo string.
 */
CXX_C_API const char *turbo_uri_userinfo(const uri_t *uri);

/**
 * @brief Get the host part of a URI.
 * @param uri Pointer to the URI structure.
 * @return Pointer to the host string.
 */
CXX_C_API const char *turbo_uri_host(const uri_t *uri);

/**
 * @brief Get the port number of a URI.
 * @param uri Pointer to the URI structure.
 * @return Port number, or 0 if not specified.
 */
CXX_C_API int turbo_uri_port(const uri_t *uri);

/**
 * @brief Get the path part of a URI.
 * @param uri Pointer to the URI structure.
 * @return Pointer to the path string.
 */
CXX_C_API const char *turbo_uri_path(const uri_t *uri);

/**
 * @brief Get the query string part of a URI.
 * @param uri Pointer to the URI structure.
 * @return Pointer to the query string.
 */
CXX_C_API const char *turbo_uri_query(const uri_t *uri);

/**
 * @brief Get the fragment (anchor) part of a URI.
 * @param uri Pointer to the URI structure.
 * @return Pointer to the fragment string.
 */
CXX_C_API const char *turbo_uri_fragment(const uri_t *uri);

/**
 * @brief Get the type of the host in the URI (e.g., IPv4, IPv6, or name).
 * @param uri Pointer to the URI structure.
 * @return The host type code.
 */
CXX_C_API turbo_uri_host_type_t turbo_uri_host_type(const uri_t *uri);

/**
 * @brief Verify if the parsed URI is semantically valid.
 * @param uri Pointer to the URI structure.
 * @return true if valid, false otherwise.
 */
CXX_C_API bool turbo_uri_is_valid(const uri_t *uri);

/* LTV Parser */
typedef struct ltv_message_s turbo_ltv_message_t;

/**
 * @brief Parse LTV (Length-Type-Value) message.
 * @param data Input buffer.
 * @param len Buffer length.
 * @param out Address of a pointer (turbo_ltv_message_t **) to store the result.
 * @return 0 on success, error code otherwise.
 */
CXX_C_API int turbo_parse_ltv(const uint8_t *data, size_t len, void *out);

/**
 * @brief Free LTV data and set pointer to NULL.
 * @param out Address of the pointer (turbo_ltv_message_t **) to free.
 */
CXX_C_API void turbo_free_ltv(void *out);

/**
 * @brief Get the type of an LTV message.
 * @param msg Pointer to the LTV message.
 * @return The message type.
 */
CXX_C_API uint8_t turbo_ltv_type(const turbo_ltv_message_t *msg);

/**
 * @brief Get a pointer to the value part of an LTV message.
 * @param msg Pointer to the LTV message.
 * @return Pointer to the value data.
 */
CXX_C_API const uint8_t *turbo_ltv_value(const turbo_ltv_message_t *msg);

/**
 * @brief Get the length of the value part of an LTV message.
 * @param msg Pointer to the LTV message.
 * @return The value length in bytes.
 */
CXX_C_API size_t turbo_ltv_value_len(const turbo_ltv_message_t *msg);

/**
 * @brief Calculate the total wire size required for an LTV message with a given value size.
 * @param value_size Number of bytes in the value part.
 * @return Total size in bytes including header.
 */
CXX_C_API size_t turbo_ltv_wire_size(size_t value_size);

/**
 * @brief Serialize an LTV message into a buffer.
 * @param type Message type.
 * @param value Pointer to value data.
 * @param value_size Length of value data.
 * @param out Output buffer.
 * @param out_len Maximum output buffer size.
 * @return Number of bytes written to the buffer.
 */
CXX_C_API size_t turbo_ltv_build(uint8_t type, const uint8_t *value, size_t value_size,
                                 uint8_t *out, size_t out_len);

/**
 * @brief Peek into a buffer to determine the total size of an LTV message.
 * @param data Input buffer.
 * @param len Available buffer length.
 * @param out_length Pointer to store the detected total message size.
 * @param out_header Pointer to store the detected header size.
 * @return 0 on success, error code otherwise.
 */
CXX_C_API int turbo_ltv_peek_size(const uint8_t *data, size_t len, uint32_t *out_length,
                                  size_t *out_header);

/* LTV Streaming */
typedef struct ltv_stream_s turbo_ltv_stream_t;

/**
 * @brief Create an LTV streaming parser.
 * @param buffer_size Size of the internal reassembly buffer.
 * @return Pointer to the new LTV stream parser.
 */
CXX_C_API turbo_ltv_stream_t *turbo_ltv_stream_create(size_t buffer_size);

/**
 * @brief Destroy an LTV streaming parser and free its resources.
 * @param stream Pointer to the stream parser to destroy.
 */
CXX_C_API void turbo_ltv_stream_destroy(turbo_ltv_stream_t *stream);

/**
 * @brief Feed incoming data to the LTV streaming parser.
 * @param stream Pointer to the stream parser.
 * @param data New data to process.
 * @param len Length of new data.
 * @param out Pointer to store a pointer to the reassembled message when complete.
 * @return 0 if a message was completed and stored in 'out', negative for error, positive if more data is needed.
 */
CXX_C_API int turbo_ltv_stream_feed(turbo_ltv_stream_t *stream, const uint8_t *data, size_t len,
                                    void **out);

/**
 * @brief Reset the internal state of the LTV streaming parser.
 * @param stream Pointer to the stream parser.
 */
CXX_C_API void turbo_ltv_stream_reset(turbo_ltv_stream_t *stream);

/* SOA Parser */
typedef struct soa_batch_s turbo_soa_batch_t;
typedef struct soa_schema_s turbo_soa_schema_t;

/**
 * @brief Parse SOA (Struct-of-Arrays) batch data.
 * @param data Input buffer.
 * @param len Buffer length.
 * @param out Address of a pointer (turbo_soa_batch_t **) to store the result.
 * @return 0 on success, error code otherwise.
 */
CXX_C_API int turbo_parse_soa(const uint8_t *data, size_t len, void *out);

/**
 * @brief Free SOA data and set pointer to NULL.
 * @param out Address of the pointer (turbo_soa_batch_t **) to free.
 */
CXX_C_API void turbo_free_soa(void *out);

/**
 * @brief Get the number of rows (entries) in an SOA batch.
 * @param batch Pointer to the SOA batch.
 * @return Row count.
 */
CXX_C_API uint32_t turbo_soa_count(const turbo_soa_batch_t *batch);

/**
 * @brief Get the schema ID associated with an SOA batch.
 * @param batch Pointer to the SOA batch.
 * @return Schema ID.
 */
CXX_C_API uint16_t turbo_soa_schema_id(const turbo_soa_batch_t *batch);

/**
 * @brief Get the field presence mask for an SOA batch.
 * @param batch Pointer to the SOA batch.
 * @return 16-bit presence mask.
 */
CXX_C_API uint16_t turbo_soa_present_mask(const turbo_soa_batch_t *batch);

/**
 * @brief Get an 8-bit integer value from a specific column and row in an SOA batch.
 * @param b Pointer to the SOA batch.
 * @param col Column index.
 * @param row Row index.
 * @return The value at the specified position.
 */
CXX_C_API int8_t turbo_soa_get_i8(const turbo_soa_batch_t *b, int col, uint32_t row);

/**
 * @brief Get an unsigned 8-bit integer value from a specific column and row in an SOA batch.
 * @param b Pointer to the SOA batch.
 * @param col Column index.
 * @param row Row index.
 * @return The value at the specified position.
 */
CXX_C_API uint8_t turbo_soa_get_u8(const turbo_soa_batch_t *b, int col, uint32_t row);

/**
 * @brief Get a 16-bit integer value from a specific column and row in an SOA batch.
 * @param b Pointer to the SOA batch.
 * @param col Column index.
 * @param row Row index.
 * @return The value at the specified position.
 */
CXX_C_API int16_t turbo_soa_get_i16(const turbo_soa_batch_t *b, int col, uint32_t row);

/**
 * @brief Get an unsigned 16-bit integer value from a specific column and row in an SOA batch.
 * @param b Pointer to the SOA batch.
 * @param col Column index.
 * @param row Row index.
 * @return The value at the specified position.
 */
CXX_C_API uint16_t turbo_soa_get_u16(const turbo_soa_batch_t *b, int col, uint32_t row);

/**
 * @brief Get a 32-bit integer value from a specific column and row in an SOA batch.
 * @param b Pointer to the SOA batch.
 * @param col Column index.
 * @param row Row index.
 * @return The value at the specified position.
 */
CXX_C_API int32_t turbo_soa_get_i32(const turbo_soa_batch_t *b, int col, uint32_t row);

/**
 * @brief Get an unsigned 32-bit integer value from a specific column and row in an SOA batch.
 * @param b Pointer to the SOA batch.
 * @param col Column index.
 * @param row Row index.
 * @return The value at the specified position.
 */
CXX_C_API uint32_t turbo_soa_get_u32(const turbo_soa_batch_t *b, int col, uint32_t row);

/**
 * @brief Get a 64-bit integer value from a specific column and row in an SOA batch.
 * @param b Pointer to the SOA batch.
 * @param col Column index.
 * @param row Row index.
 * @return The value at the specified position.
 */
CXX_C_API int64_t turbo_soa_get_i64(const turbo_soa_batch_t *b, int col, uint32_t row);

/**
 * @brief Get an unsigned 64-bit integer value from a specific column and row in an SOA batch.
 * @param b Pointer to the SOA batch.
 * @param col Column index.
 * @param row Row index.
 * @return The value at the specified position.
 */
CXX_C_API uint64_t turbo_soa_get_u64(const turbo_soa_batch_t *b, int col, uint32_t row);

/**
 * @brief Get a double value from a specific column and row in an SOA batch.
 * @param b Pointer to the SOA batch.
 * @param col Column index.
 * @param row Row index.
 * @return The value at the specified position.
 */
CXX_C_API double turbo_soa_get_f64(const turbo_soa_batch_t *b, int col, uint32_t row);

/**
 * @brief Calculate the wire size required for an SOA batch with given schema, count, and mask.
 * @param schema Batch schema.
 * @param count Number of rows.
 * @param present_mask Field presence mask.
 * @return Required size in bytes.
 */
CXX_C_API size_t turbo_soa_wire_size(const turbo_soa_schema_t *schema, uint32_t count,
                                     uint16_t present_mask);

/**
 * @brief Build the header for an SOA batch into a buffer.
 * @param schema Batch schema.
 * @param count Number of rows.
 * @param present_mask Field presence mask.
 * @param out Output buffer.
 * @param out_len Maximum output buffer size.
 * @return Number of bytes written to the buffer.
 */
CXX_C_API size_t turbo_soa_build_header(const turbo_soa_schema_t *schema, uint32_t count,
                                        uint16_t present_mask, uint8_t *out, size_t out_len);

/**
 * @brief Get the hardware width for a given SOA data type.
 * @param type Data type code.
 * @return Width in bytes.
 */
CXX_C_API uint8_t turbo_soa_type_width(int type);

/**
 * @brief Peek into a buffer to determine the count and schema of an SOA batch.
 * @param data Input buffer.
 * @param len Available buffer length.
 * @param out_count Pointer to store the row count.
 * @param out_schema Pointer to store the schema ID.
 * @return 0 on success, error code otherwise.
 */
CXX_C_API int turbo_soa_peek_header(const uint8_t *data, size_t len, uint32_t *out_count,
                                    uint16_t *out_schema);

/**
 * @brief Get the number of columns defined by a schema.
 * @param schema Pointer to schema.
 * @return Column count.
 */
CXX_C_API int turbo_soa_schema_count(const turbo_soa_schema_t *schema);

/**
 * @brief Get the data type of a column in a schema.
 * @param schema Pointer to schema.
 * @param idx Column index.
 * @return Data type code.
 */
CXX_C_API int turbo_soa_schema_column_type(const turbo_soa_schema_t *schema, int idx);

/* TOON Parser */
typedef struct toonObject turbo_toon_node_t;

typedef enum {
  TURBO_TOON_STRING = 0,
  TURBO_TOON_INT = 1,
  TURBO_TOON_BOOL = 2,
  TURBO_TOON_NULL = 3,
  TURBO_TOON_DOUBLE = 4,
  TURBO_TOON_OBJECT = 5,
  TURBO_TOON_LIST = 6
} turbo_toon_type_t;

/**
 * @brief Parse TOON (Turbo Object Notation) data.
 * @param data Input buffer.
 * @param len Buffer length.
 * @param out Address of a pointer (turbo_toon_node_t **) to store the result.
 * @return 0 on success, error code otherwise.
 */
CXX_C_API int turbo_parse_toon(const uint8_t *data, size_t len, void *out);

/**
 * @brief Free TOON data and set pointer to NULL.
 * @param out Address of the pointer (turbo_toon_node_t **) to free.
 */
CXX_C_API void turbo_free_toon(void *out);

/**
 * @brief Get the type of a TOON node.
 * @param node Pointer to the TOON node.
 * @return The node type code.
 */
CXX_C_API turbo_toon_type_t turbo_toon_type(const turbo_toon_node_t *node);

/**
 * @brief Check if a TOON node represents a null value.
 * @param node Pointer to the TOON node.
 * @return true if null, false otherwise.
 */
CXX_C_API bool turbo_toon_is_null(const turbo_toon_node_t *node);

/**
 * @brief Get boolean value from a TOON boolean node.
 * @param node Pointer to the TOON node.
 * @return The boolean value.
 */
CXX_C_API bool turbo_toon_bool(const turbo_toon_node_t *node);

/**
 * @brief Get numeric value from a TOON node.
 * @param node Pointer to the TOON node.
 * @return The numeric value as a double.
 */
CXX_C_API double turbo_toon_number(const turbo_toon_node_t *node);

/**
 * @brief Get integer value from a TOON node.
 * @param node Pointer to the TOON node.
 * @return The numeric value as an integer.
 */
CXX_C_API int turbo_toon_int(const turbo_toon_node_t *node);

/**
 * @brief Get string value from a TOON string node.
 * @param node Pointer to the TOON node.
 * @return Pointer to the string data.
 */
CXX_C_API const char *turbo_toon_string(const turbo_toon_node_t *node);

/**
 * @brief Get the length of a TOON string node.
 * @param node Pointer to the TOON node.
 * @return The length of the string in bytes.
 */
CXX_C_API size_t turbo_toon_string_len(const turbo_toon_node_t *node);

/**
 * @brief Navigate to a child node using a path string (e.g., "server.host").
 * @param root Pointer to the root TOON node.
 * @param path Path string.
 * @return Pointer to the target node if found, NULL otherwise.
 */
CXX_C_API turbo_toon_node_t *turbo_toon_get(turbo_toon_node_t *root, const char *path);

/**
 * @brief Get the number of elements in a TOON array (list) node.
 * @param arr Pointer to the TOON array node.
 * @return Number of elements.
 */
CXX_C_API size_t turbo_toon_array_size(const turbo_toon_node_t *arr);

/**
 * @brief Get an element from a TOON array node by index.
 * @param arr Pointer to the TOON array node.
 * @param index Element index.
 * @return Pointer to the element node.
 */
CXX_C_API turbo_toon_node_t *turbo_toon_array_get(const turbo_toon_node_t *arr, size_t index);

/**
 * @brief Serialize a TOON node to its string representation.
 * @param node Pointer to the TOON node.
 * @param out_len Optional pointer to store the output string length.
 * @return Pointer to the allocated string (must be freed with turbo_toon_serialize_free).
 */
CXX_C_API char *turbo_toon_serialize(const turbo_toon_node_t *node, size_t *out_len);

/**
 * @brief Free a string allocated by turbo_toon_serialize.
 * @param str Pointer to the serialized string.
 */
CXX_C_API void turbo_toon_serialize_free(char *str);

/**
 * @brief Serialize a TOON node to a JSON formatted string.
 * @param node Pointer to the TOON node.
 * @param out_len Optional pointer to store the output string length.
 * @return Pointer to the allocated string (must be freed with turbo_toon_serialize_json_free).
 */
CXX_C_API char *turbo_toon_serialize_json(const turbo_toon_node_t *node, size_t *out_len);

/**
 * @brief Free a string allocated by turbo_toon_serialize_json.
 * @param str Pointer to the JSON string.
 */
CXX_C_API void turbo_toon_serialize_json_free(char *str);

/**
 * @brief Parse a JSON string into a TOON structure.
 * @param json Input JSON string.
 * @param len JSON string length.
 * @return Pointer to the root TOON node.
 */
CXX_C_API turbo_toon_node_t *turbo_toon_from_json(const char *json, size_t len);

/* CMD Parser */
typedef struct turbo_cmd_parser_s turbo_cmd_parser_t;
typedef struct turbo_cmd_subcommand_s turbo_cmd_subcommand_t;

/* Enum choice for turbo_cmd_add_enum */
typedef struct {
  const char *name;
  const char *info;
  int64_t value;
} turbo_cmd_enum_t;

/* Custom validator signature */
typedef bool (*turbo_cmd_validator_t)(const char *value, const char **error_message);

/**
 * @brief Create a command line argument parser.
 * @param app_name Name of the application.
 * @param version Application version string.
 * @return Pointer to the new command parser.
 */
CXX_C_API turbo_cmd_parser_t *turbo_cmd_create(const char *app_name, const char *version);

/**
 * @brief Destroy a command line argument parser and free its resources.
 * @param parser Pointer to the command parser.
 */
CXX_C_API void turbo_cmd_destroy(turbo_cmd_parser_t *parser);

/* Basic argument types */
/**
 * @brief Add a boolean flag argument.
 * @param parser Pointer to the command parser.
 * @param out Pointer to store the result (true if flag present).
 * @param name Long name (e.g., "--verbose").
 * @param short_name Short name (e.g., "-v").
 * @param desc Argument description for help message.
 */
CXX_C_API void turbo_cmd_add_flag(turbo_cmd_parser_t *parser, bool *out, const char *name,
                                  const char *short_name, const char *desc);

/**
 * @brief Add a string argument.
 * @param parser Pointer to the command parser.
 * @param out Pointer to store the result string address.
 * @param name Long name.
 * @param short_name Short name.
 * @param desc Description.
 */
CXX_C_API void turbo_cmd_add_string(turbo_cmd_parser_t *parser, char **out, const char *name,
                                    const char *short_name, const char *desc);

/**
 * @brief Add an integer argument.
 * @param parser Pointer to the command parser.
 * @param out Pointer to store the numeric result.
 * @param name Long name.
 * @param short_name Short name.
 * @param desc Description.
 */
CXX_C_API void turbo_cmd_add_integer(turbo_cmd_parser_t *parser, int64_t *out, const char *name,
                                     const char *short_name, const char *desc);

/**
 * @brief Add a floating point argument.
 * @param parser Pointer to the command parser.
 * @param out Pointer to store the numeric result.
 * @param name Long name.
 * @param short_name Short name.
 * @param desc Description.
 */
CXX_C_API void turbo_cmd_add_float(turbo_cmd_parser_t *parser, double *out, const char *name,
                                   const char *short_name, const char *desc);

/**
 * @brief Add an argument that accepts a list of strings.
 * @param parser Pointer to the command parser.
 * @param out_arr Array to store result string addresses.
 * @param out_count Pointer to store actual count of strings received.
 * @param max_count Maximum size of out_arr.
 * @param name Long name.
 * @param short_name Short name.
 * @param desc Description.
 */
CXX_C_API void turbo_cmd_add_string_list(turbo_cmd_parser_t *parser, char **out_arr,
                                         uint32_t *out_count, uint32_t max_count, const char *name,
                                         const char *short_name, const char *desc);

/**
 * @brief Add an argument constrained to a set of enumerated choices.
 * @param parser Pointer to the command parser.
 * @param out Pointer to store the selected value (choice ID).
 * @param name Long name.
 * @param short_name Short name.
 * @param desc Description.
 * @param choices Array of valid options.
 * @param choices_count Size of choices array.
 */
CXX_C_API void turbo_cmd_add_enum(turbo_cmd_parser_t *parser, int64_t *out, const char *name,
                                  const char *short_name, const char *desc,
                                  turbo_cmd_enum_t *choices, uint32_t choices_count);

/* Required positional arguments */
/**
 * @brief Add a required positional string argument.
 * @param parser Pointer to the command parser.
 * @param out Pointer to store the string result.
 * @param name Internal identifier name.
 * @param desc Description.
 */
CXX_C_API void turbo_cmd_add_required_string(turbo_cmd_parser_t *parser, char **out,
                                             const char *name, const char *desc);

/**
 * @brief Add a required positional integer argument.
 * @param parser Pointer to the command parser.
 * @param out Pointer to store the numeric result.
 * @param name Internal identifier name.
 * @param desc Description.
 */
CXX_C_API void turbo_cmd_add_required_integer(turbo_cmd_parser_t *parser, int64_t *out,
                                              const char *name, const char *desc);

/* Argument modifiers (return index for chaining) */
/**
 * @brief Bind a command line argument to an environment variable.
 * @param parser Pointer to the command parser.
 * @param index Index of the argument to bind.
 * @param env_var Name of the environment variable.
 */
CXX_C_API void turbo_cmd_set_env(turbo_cmd_parser_t *parser, uint32_t index, const char *env_var);

/**
 * @brief Assign an argument to a logical group for help formatting.
 * @param parser Pointer to the command parser.
 * @param index Index of the argument.
 * @param group Group name.
 */
CXX_C_API void turbo_cmd_set_group(turbo_cmd_parser_t *parser, uint32_t index, const char *group);

/**
 * @brief Restrict a string argument to a fixed set of valid choices.
 * @param parser Pointer to the command parser.
 * @param index Index of the string argument.
 * @param choices Array of valid string choices.
 * @param count Number of choices in the array.
 */
CXX_C_API void turbo_cmd_set_choices(turbo_cmd_parser_t *parser, uint32_t index,
                                     const char **choices, uint32_t count);

/**
 * @brief Attach a custom validation function to an argument.
 * @param parser Pointer to the command parser.
 * @param index Index of the argument.
 * @param validator Pointer to the validator function.
 */
CXX_C_API void turbo_cmd_set_validator(turbo_cmd_parser_t *parser, uint32_t index,
                                       turbo_cmd_validator_t validator);

/**
 * @brief Mark an optional argument as required.
 * @param parser Pointer to the command parser.
 * @param index Index of the argument.
 */
CXX_C_API void turbo_cmd_set_required(turbo_cmd_parser_t *parser, uint32_t index);

/* Get last added argument index (for modifier chaining) */
/**
 * @brief Get the numeric index of the most recently added argument.
 * @param parser Pointer to the command parser.
 * @return The 0-based index of the last argument added.
 */
CXX_C_API uint32_t turbo_cmd_last_index(turbo_cmd_parser_t *parser);

/* Subcommand support */
/**
 * @brief Add a subcommand to the parser (e.g., "commit" for "git").
 * @param parser Pointer to the command parser.
 * @param name Subcommand name.
 * @param desc Subcommand description.
 * @return Pointer to the new subcommand object.
 */
CXX_C_API turbo_cmd_subcommand_t *turbo_cmd_add_subcommand(turbo_cmd_parser_t *parser,
                                                           const char *name, const char *desc);

/**
 * @brief Add a boolean flag to a subcommand.
 * @param sub Pointer to the subcommand.
 * @param out Pointer to store the result.
 * @param name Long name.
 * @param short_name Short name.
 * @param desc Description.
 */
CXX_C_API void turbo_cmd_sub_add_flag(turbo_cmd_subcommand_t *sub, bool *out, const char *name,
                                      const char *short_name, const char *desc);

/**
 * @brief Add a string argument to a subcommand.
 * @param sub Pointer to the subcommand.
 * @param out Pointer to store the result string address.
 * @param name Long name.
 * @param short_name Short name.
 * @param desc Description.
 */
CXX_C_API void turbo_cmd_sub_add_string(turbo_cmd_subcommand_t *sub, char **out, const char *name,
                                        const char *short_name, const char *desc);

/**
 * @brief Add an integer argument to a subcommand.
 * @param sub Pointer to the subcommand.
 * @param out Pointer to store the numeric result.
 * @param name Long name.
 * @param short_name Short name.
 * @param desc Description.
 */
CXX_C_API void turbo_cmd_sub_add_integer(turbo_cmd_subcommand_t *sub, int64_t *out,
                                         const char *name, const char *short_name,
                                         const char *desc);

/**
 * @brief Add a required positional string argument to a subcommand.
 * @param sub Pointer to the subcommand.
 * @param out Pointer to store the string result.
 * @param name Internal identifier name.
 * @param desc Description.
 */
CXX_C_API void turbo_cmd_sub_add_required_string(turbo_cmd_subcommand_t *sub, char **out,
                                                 const char *name, const char *desc);

/* Parsing */
/**
 * @brief Parse the command line arguments.
 * @param parser Pointer to the command parser.
 * @param argc Number of arguments.
 * @param argv Array of argument strings.
 * @param colors true to enable colored help output.
 */
CXX_C_API void turbo_cmd_parse(turbo_cmd_parser_t *parser, int argc, char **argv, bool colors);

/**
 * @brief Parse arguments starting from a subcommand.
 * @param parser Pointer to the command parser.
 * @param argc Number of arguments.
 * @param argv Array of argument strings.
 * @param colors true to enable colored output.
 * @return 0 on success, non-zero if internal error occurs.
 */
CXX_C_API int turbo_cmd_parse_subcommand(turbo_cmd_parser_t *parser, int argc, char **argv,
                                         bool colors);

/**
 * @brief Display the auto-generated help documentation to stdout.
 * @param parser Pointer to the command parser.
 * @param colors true to enable colored output.
 */
CXX_C_API void turbo_cmd_show_help(turbo_cmd_parser_t *parser, bool colors);

/* DotEnv Parser */
/**
 * @brief Load environment variables from a specific .env file.
 * @param path Path to the .env file.
 * @param overwrite true to overwrite existing environment variables.
 * @return 0 on success, negative error code otherwise.
 */
CXX_C_API int turbo_dotenv_load(const char *path, bool overwrite);

/**
 * @brief Load environment variables from the default ".env" file in CWD.
 * @param overwrite true to overwrite existing environment variables.
 * @return 0 on success, negative error code otherwise.
 */
CXX_C_API int turbo_dotenv_load_default(bool overwrite);

/* Datetime Parser */
typedef struct {
  int year;        // e.g., 2006
  int month;       // 1-12
  int day;         // 1-31
  int hour;        // 0-23
  int minute;      // 0-59
  int second;      // 0-59
  int millisecond; // 0-999
  int tz_offset;   // in minutes from UTC (e.g., +03:45 -> 225)
  int has_tz;      // bool
  int day_of_week; // 0-6 (Sun-Sat), -1 if not set
} turbo_datetime_t;

/**
 * @brief Parses a date-time string in various formats (RFC-822, ISO-8601, HTTP, NCSA, etc.).
 * Supported formats:
 * - Compact: YYYYMMDD, HHMMSS, etc.
 * - ISO-8601: 2006-03-14T13:27:54+03:45
 * - RFC-822 / HTTP: Sat, 04 Mar 2006 13:27:54 GMT
 * - NCSA: 04/Mar/2006:13:27:54 -0500
 * - Various common slash, hyphen, and dot separated formats.
 *
 * @param str The date string to parse.
 * @param len Length of the string.
 * @param out Output datetime structure.
 * @return 0 on success, -1 on failure.
 */
CXX_C_API int turbo_parse_datetime(const char *str, size_t len, turbo_datetime_t *out);

/**
 * @brief Converts turbo_datetime_t to time_t (UTC).
 *
 * @param dt Input datetime structure.
 * @return time_t value (seconds since epoch), or -1 on error.
 */
CXX_C_API time_t turbo_datetime_to_time(const turbo_datetime_t *dt);

/**
 * @brief Formats a time_t as an RFC 7231 / RFC 822 HTTP date-time string.
 *
 * @param t Time to format.
 * @param buf Output buffer (at least 30 bytes).
 * @param buf_len Size of the buffer.
 * @return Number of characters written, or -1 on failure.
 */
CXX_C_API int turbo_datetime_format_rfc822(time_t t, char *buf, size_t buf_len);

/* TOML Parser */
typedef struct toml_table_t turbo_toml_t;
typedef struct toml_array_t turbo_toml_array_t;

typedef struct {
  char kind;
  int year, month, day;
  int hour, minute, second, millisec;
  int tz;
} turbo_toml_timestamp_t;

typedef struct {
  bool ok;
  union {
    struct {
      char *s;
      int sl;
    };
    turbo_toml_timestamp_t ts;
    bool b;
    int64_t i;
    double d;
  } u;
} turbo_toml_value_t;

/**
 * @brief Parse TOML formatted data.
 * @param data Input buffer.
 * @param len Buffer length.
 * @param out Address of a pointer (turbo_toml_t **) to store the result.
 * @return 0 on success, error code otherwise.
 */
CXX_C_API int turbo_parse_toml(const uint8_t *data, size_t len, void *out);

/**
 * @brief Free TOML data and set pointer to NULL.
 * @param out Address of the pointer (turbo_toml_t **) to free.
 */
CXX_C_API void turbo_free_toml(void *out);

/**
 * @brief Get the number of entries in a TOML table.
 * @param table Pointer to TOML table.
 * @return Number of entries.
 */
CXX_C_API int turbo_toml_len(const turbo_toml_t *table);

/**
 * @brief Get the key name at a specific index in a TOML table.
 * @param table Pointer to TOML table.
 * @param index Entry index.
 * @param keylen Optional pointer to store key string length.
 * @return Key name string.
 */
CXX_C_API const char *turbo_toml_key(const turbo_toml_t *table, int index, int *keylen);

/**
 * @brief Get a string value from a TOML table by key.
 * @param table Pointer to TOML table.
 * @param key Entry key.
 * @return Result value structure.
 */
CXX_C_API turbo_toml_value_t turbo_toml_string(const turbo_toml_t *table, const char *key);

/**
 * @brief Get a boolean value from a TOML table by key.
 * @param table Pointer to TOML table.
 * @param key Entry key.
 * @return Result value structure.
 */
CXX_C_API turbo_toml_value_t turbo_toml_bool(const turbo_toml_t *table, const char *key);

/**
 * @brief Get an integer value from a TOML table by key.
 * @param table Pointer to TOML table.
 * @param key Entry key.
 * @return Result value structure.
 */
CXX_C_API turbo_toml_value_t turbo_toml_int(const turbo_toml_t *table, const char *key);

/**
 * @brief Get a floating-point value from a TOML table by key.
 * @param table Pointer to TOML table.
 * @param key Entry key.
 * @return Result value structure.
 */
CXX_C_API turbo_toml_value_t turbo_toml_double(const turbo_toml_t *table, const char *key);

/**
 * @brief Get a timestamp value from a TOML table by key.
 * @param table Pointer to TOML table.
 * @param key Entry key.
 * @return Result value structure.
 */
CXX_C_API turbo_toml_value_t turbo_toml_timestamp(const turbo_toml_t *table, const char *key);

/**
 * @brief Get a sub-array from a TOML table by key.
 * @param table Pointer to TOML table.
 * @param key Entry key.
 * @return Pointer to TOML array if found, NULL otherwise.
 */
CXX_C_API turbo_toml_array_t *turbo_toml_array(const turbo_toml_t *table, const char *key);

/**
 * @brief Get a nested table from a TOML table by key.
 * @param table Pointer to TOML table.
 * @param key Entry key.
 * @return Pointer to TOML table if found, NULL otherwise.
 */
CXX_C_API turbo_toml_t *turbo_toml_table(const turbo_toml_t *table, const char *key);

/**
 * @brief Get the number of elements in a TOML array.
 * @param array Pointer to TOML array.
 * @return Element count.
 */
CXX_C_API int turbo_toml_array_len(const turbo_toml_array_t *array);

/**
 * @brief Get a string value from a TOML array by index.
 * @param array Pointer to TOML array.
 * @param idx Element index.
 * @return Result value structure.
 */
CXX_C_API turbo_toml_value_t turbo_toml_array_string(const turbo_toml_array_t *array, int idx);

/**
 * @brief Get a boolean value from a TOML array by index.
 * @param array Pointer to TOML array.
 * @param idx Element index.
 * @return Result value structure.
 */
CXX_C_API turbo_toml_value_t turbo_toml_array_bool(const turbo_toml_array_t *array, int idx);

/**
 * @brief Get an integer value from a TOML array by index.
 * @param array Pointer to TOML array.
 * @param idx Element index.
 * @return Result value structure.
 */
CXX_C_API turbo_toml_value_t turbo_toml_array_int(const turbo_toml_array_t *array, int idx);

/**
 * @brief Get a floating-point value from a TOML array by index.
 * @param array Pointer to TOML array.
 * @param idx Element index.
 * @return Result value structure.
 */
CXX_C_API turbo_toml_value_t turbo_toml_array_double(const turbo_toml_array_t *array, int idx);

/**
 * @brief Get a timestamp value from a TOML array by index.
 * @param array Pointer to TOML array.
 * @param idx Element index.
 * @return Result value structure.
 */
CXX_C_API turbo_toml_value_t turbo_toml_array_timestamp(const turbo_toml_array_t *array, int idx);

/**
 * @brief Get a nested array from a TOML array by index.
 * @param array Pointer to TOML array.
 * @param idx Element index.
 * @return Pointer to TOML array if found, NULL otherwise.
 */
CXX_C_API turbo_toml_array_t *turbo_toml_array_array(const turbo_toml_array_t *array, int idx);

/**
 * @brief Get a nested table from a TOML array by index.
 * @param array Pointer to TOML array.
 * @param idx Element index.
 * @return Pointer to TOML table if found, NULL otherwise.
 */
CXX_C_API turbo_toml_t *turbo_toml_array_table(const turbo_toml_array_t *array, int idx);
#ifdef __cplusplus
}
#endif
#endif // TURBO_PARSER_H
