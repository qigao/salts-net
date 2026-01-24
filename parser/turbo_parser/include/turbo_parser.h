#ifndef TURBO_PARSER_H
#define TURBO_PARSER_H

#include "platform.h"

#include <stdbool.h>
#include <stddef.h>

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

CXX_C_API int turbo_parse_json(const uint8_t *data, size_t len, void *out);
CXX_C_API void turbo_free_json(void *out);

CXX_C_API turbo_json_type_t turbo_json_type(const json_value_t *value);
CXX_C_API bool turbo_json_is_null(const json_value_t *value);
CXX_C_API bool turbo_json_bool(const json_value_t *value);
CXX_C_API double turbo_json_number(const json_value_t *value);
CXX_C_API const char *turbo_json_string(const json_value_t *value);
CXX_C_API size_t turbo_json_string_len(const json_value_t *value);

CXX_C_API size_t turbo_json_object_size(const json_value_t *obj);
CXX_C_API const char *turbo_json_object_key(const json_value_t *obj, size_t index);
CXX_C_API json_value_t *turbo_json_object_value(const json_value_t *obj, size_t index);
CXX_C_API json_value_t *turbo_json_object_get(const json_value_t *obj, const char *key);

CXX_C_API size_t turbo_json_array_size(const json_value_t *arr);
CXX_C_API json_value_t *turbo_json_array_get(const json_value_t *arr, size_t index);

CXX_C_API int turbo_json_get_int(const json_value_t *obj, const char *key, int def);
CXX_C_API bool turbo_json_get_bool(const json_value_t *obj, const char *key, bool def);
CXX_C_API double turbo_json_get_double(const json_value_t *obj, const char *key, double def);
CXX_C_API const char *turbo_json_get_string(const json_value_t *obj, const char *key);

CXX_C_API char *turbo_json_serialize(const json_value_t *value, size_t *out_len);
CXX_C_API void turbo_json_serialize_free(char *str);

/* JSON Builder/Modifier */
CXX_C_API json_value_t *turbo_json_create_object(void);
CXX_C_API json_value_t *turbo_json_create_array(void);
CXX_C_API json_value_t *turbo_json_create_string(const char *str);
CXX_C_API json_value_t *turbo_json_create_number(double num);
CXX_C_API json_value_t *turbo_json_create_bool(bool val);
CXX_C_API json_value_t *turbo_json_create_null(void);

CXX_C_API void turbo_json_object_add(json_value_t *obj, const char *key, json_value_t *val);
CXX_C_API void turbo_json_array_add(json_value_t *arr, json_value_t *val);

CXX_C_API void turbo_json_object_set_string(json_value_t *obj, const char *key, const char *val);
CXX_C_API void turbo_json_object_set_number(json_value_t *obj, const char *key, double val);
CXX_C_API void turbo_json_object_set_bool(json_value_t *obj, const char *key, bool val);
CXX_C_API void turbo_json_object_set_null(json_value_t *obj, const char *key);

/* CSV */
typedef struct csv_doc_s turbo_csv_doc_t;

CXX_C_API int turbo_parse_csv(const uint8_t *data, size_t len, void *out);
CXX_C_API void turbo_free_csv(void *out);

CXX_C_API size_t turbo_csv_row_count(const turbo_csv_doc_t *doc);
CXX_C_API size_t turbo_csv_column_count(const turbo_csv_doc_t *doc);
CXX_C_API const char *turbo_csv_get(const turbo_csv_doc_t *doc, size_t row, size_t col);
CXX_C_API int turbo_csv_get_int(const turbo_csv_doc_t *doc, size_t row, size_t col, int def);
CXX_C_API double turbo_csv_get_double(const turbo_csv_doc_t *doc, size_t row, size_t col,
                                      double def);
CXX_C_API bool turbo_csv_get_bool(const turbo_csv_doc_t *doc, size_t row, size_t col, bool def);

/* INI */
typedef struct ini_s turbo_ini_t;

CXX_C_API int turbo_parse_ini(const uint8_t *data, size_t len, void *out);
CXX_C_API void turbo_free_ini(void *out);

CXX_C_API const char *turbo_ini_get(const turbo_ini_t *ini, const char *section, const char *key);
CXX_C_API int turbo_ini_get_int(const turbo_ini_t *ini, const char *section, const char *key,
                                int def);
CXX_C_API bool turbo_ini_get_bool(const turbo_ini_t *ini, const char *section, const char *key,
                                  bool def);
CXX_C_API double turbo_ini_get_double(const turbo_ini_t *ini, const char *section, const char *key,
                                      double def);

/* TLV */
typedef struct frame_s turbo_tlv_frame_t;

CXX_C_API int turbo_parse_tlv(const uint8_t *data, size_t len, void *out);
CXX_C_API void turbo_free_tlv(void *out);

CXX_C_API uint32_t turbo_tlv_msg_id(const turbo_tlv_frame_t *frame);
CXX_C_API uint8_t turbo_tlv_version(const turbo_tlv_frame_t *frame);
CXX_C_API uint8_t turbo_tlv_type(const turbo_tlv_frame_t *frame);
CXX_C_API size_t turbo_tlv_payload_size(const turbo_tlv_frame_t *frame);
CXX_C_API const char *turbo_tlv_payload(const turbo_tlv_frame_t *frame);
CXX_C_API uint32_t turbo_tlv_crc32(const turbo_tlv_frame_t *frame);
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

CXX_C_API int turbo_parse_uri(const uint8_t *data, size_t len, void *out);
CXX_C_API void turbo_free_uri(void *out);

CXX_C_API const char *turbo_uri_scheme(const uri_t *uri);
CXX_C_API const char *turbo_uri_userinfo(const uri_t *uri);
CXX_C_API const char *turbo_uri_host(const uri_t *uri);
CXX_C_API int turbo_uri_port(const uri_t *uri);
CXX_C_API const char *turbo_uri_path(const uri_t *uri);
CXX_C_API const char *turbo_uri_query(const uri_t *uri);
CXX_C_API const char *turbo_uri_fragment(const uri_t *uri);
CXX_C_API turbo_uri_host_type_t turbo_uri_host_type(const uri_t *uri);
CXX_C_API bool turbo_uri_is_valid(const uri_t *uri);

/* LTV Parser */
typedef struct ltv_message_s turbo_ltv_message_t;

CXX_C_API int turbo_parse_ltv(const uint8_t *data, size_t len, void *out);
CXX_C_API void turbo_free_ltv(void *out);

CXX_C_API uint8_t turbo_ltv_type(const turbo_ltv_message_t *msg);
CXX_C_API const uint8_t *turbo_ltv_value(const turbo_ltv_message_t *msg);
CXX_C_API size_t turbo_ltv_value_len(const turbo_ltv_message_t *msg);

CXX_C_API size_t turbo_ltv_wire_size(size_t value_size);
CXX_C_API size_t turbo_ltv_build(uint8_t type, const uint8_t *value, size_t value_size,
                                 uint8_t *out, size_t out_len);
CXX_C_API int turbo_ltv_peek_size(const uint8_t *data, size_t len, uint32_t *out_length,
                                  size_t *out_header);

/* LTV Streaming */
typedef struct ltv_stream_s turbo_ltv_stream_t;

CXX_C_API turbo_ltv_stream_t *turbo_ltv_stream_create(size_t buffer_size);
CXX_C_API void turbo_ltv_stream_destroy(turbo_ltv_stream_t *stream);
CXX_C_API int turbo_ltv_stream_feed(turbo_ltv_stream_t *stream, const uint8_t *data, size_t len,
                                    void **out);
CXX_C_API void turbo_ltv_stream_reset(turbo_ltv_stream_t *stream);

/* SOA Parser */
typedef struct soa_batch_s turbo_soa_batch_t;
typedef struct soa_schema_s turbo_soa_schema_t;

CXX_C_API int turbo_parse_soa(const uint8_t *data, size_t len, void *out);
CXX_C_API void turbo_free_soa(void *out);

CXX_C_API uint32_t turbo_soa_count(const turbo_soa_batch_t *batch);
CXX_C_API uint16_t turbo_soa_schema_id(const turbo_soa_batch_t *batch);
CXX_C_API uint16_t turbo_soa_present_mask(const turbo_soa_batch_t *batch);

CXX_C_API int8_t turbo_soa_get_i8(const turbo_soa_batch_t *b, int col, uint32_t row);
CXX_C_API uint8_t turbo_soa_get_u8(const turbo_soa_batch_t *b, int col, uint32_t row);
CXX_C_API int16_t turbo_soa_get_i16(const turbo_soa_batch_t *b, int col, uint32_t row);
CXX_C_API uint16_t turbo_soa_get_u16(const turbo_soa_batch_t *b, int col, uint32_t row);
CXX_C_API int32_t turbo_soa_get_i32(const turbo_soa_batch_t *b, int col, uint32_t row);
CXX_C_API uint32_t turbo_soa_get_u32(const turbo_soa_batch_t *b, int col, uint32_t row);
CXX_C_API int64_t turbo_soa_get_i64(const turbo_soa_batch_t *b, int col, uint32_t row);
CXX_C_API uint64_t turbo_soa_get_u64(const turbo_soa_batch_t *b, int col, uint32_t row);
CXX_C_API double turbo_soa_get_f64(const turbo_soa_batch_t *b, int col, uint32_t row);

CXX_C_API size_t turbo_soa_wire_size(const turbo_soa_schema_t *schema, uint32_t count,
                                     uint16_t present_mask);
CXX_C_API size_t turbo_soa_build_header(const turbo_soa_schema_t *schema, uint32_t count,
                                        uint16_t present_mask, uint8_t *out, size_t out_len);
CXX_C_API uint8_t turbo_soa_type_width(int type);
CXX_C_API int turbo_soa_peek_header(const uint8_t *data, size_t len, uint32_t *out_count,
                                    uint16_t *out_schema);

CXX_C_API int turbo_soa_schema_count(const turbo_soa_schema_t *schema);
CXX_C_API int turbo_soa_schema_column_type(const turbo_soa_schema_t *schema, int idx);

/* CMD Parser */
typedef struct turbo_cmd_parser_s turbo_cmd_parser_t;

CXX_C_API turbo_cmd_parser_t *turbo_cmd_create(const char *app_name, const char *version);
CXX_C_API void turbo_cmd_destroy(turbo_cmd_parser_t *parser);

CXX_C_API void turbo_cmd_add_flag(turbo_cmd_parser_t *parser, bool *out, const char *name,
                                  const char *short_name, const char *desc);
CXX_C_API void turbo_cmd_add_string(turbo_cmd_parser_t *parser, char **out, const char *name,
                                    const char *short_name, const char *desc);
CXX_C_API void turbo_cmd_add_integer(turbo_cmd_parser_t *parser, int64_t *out, const char *name,
                                     const char *short_name, const char *desc);
CXX_C_API void turbo_cmd_add_float(turbo_cmd_parser_t *parser, double *out, const char *name,
                                   const char *short_name, const char *desc);
CXX_C_API void turbo_cmd_add_string_list(turbo_cmd_parser_t *parser, char **out_arr,
                                         uint32_t *out_count, uint32_t max_count, const char *name,
                                         const char *short_name, const char *desc);

/* Required arguments */
CXX_C_API void turbo_cmd_add_required_string(turbo_cmd_parser_t *parser, char **out,
                                             const char *name, const char *desc);

CXX_C_API void turbo_cmd_parse(turbo_cmd_parser_t *parser, int argc, char **argv, bool colors);

/* DotEnv Parser */
CXX_C_API int turbo_dotenv_load(const char *path, bool overwrite);
CXX_C_API int turbo_dotenv_load_default(bool overwrite);

#ifdef __cplusplus
}
#endif

#endif // TURBO_PARSER_H
