/**
 * @file json_parser.h
 * @brief Lightweight JSON Parser using re2c + Lemon
 */

#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <turbo_str_view.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct json_value_s json_value_t;
typedef struct json_path_result_s json_path_result_t;

typedef enum {
  JSON_NULL,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT
} json_type_t;

json_value_t *json_parse(const char *content, size_t len);
json_value_t *json_parse_file(const char *filename);
void json_free(json_value_t *value);

json_type_t json_type(const json_value_t *value);
bool json_is_null(const json_value_t *value);
bool json_bool(const json_value_t *value);
double json_number(const json_value_t *value);
const char *json_string(const json_value_t *value);
size_t json_string_len(const json_value_t *value);
tstr_v json_string_v(const json_value_t *value);

size_t json_object_size(const json_value_t *obj);
const char *json_object_key(const json_value_t *obj, size_t index);
size_t json_object_key_len(const json_value_t *obj, size_t index);
tstr_v json_object_key_v(const json_value_t *obj, size_t index);
json_value_t *json_object_value(const json_value_t *obj, size_t index);
json_value_t *json_object_get(const json_value_t *obj, const char *key);
json_value_t *json_object_get_v(const json_value_t *obj, tstr_v key);

size_t json_array_size(const json_value_t *arr);
json_value_t *json_array_get(const json_value_t *arr, size_t index);

int json_get_int(const json_value_t *obj, const char *key, int def);
bool json_get_bool(const json_value_t *obj, const char *key, bool def);
double json_get_double(const json_value_t *obj, const char *key, double def);
const char *json_get_string(const json_value_t *obj, const char *key);
tstr_v json_get_string_v(const json_value_t *obj, const char *key);

int json_get_int_v(const json_value_t *obj, tstr_v key, int def);
bool json_get_bool_v(const json_value_t *obj, tstr_v key, bool def);
double json_get_double_v(const json_value_t *obj, tstr_v key, double def);
tstr_v json_get_string_vv(const json_value_t *obj, tstr_v key);

const char *json_get_error(void);
char *json_serialize(const json_value_t *value, size_t *out_len);
char *json_serialize_pretty(const json_value_t *value, size_t *out_len);
char *json_serialize_pretty_crlf(const json_value_t *value, size_t *out_len);
void json_serialize_free(char *str);

/* ============================================================================
 * JSONPath Query API
 * ============================================================================ */

json_value_t *json_path_get(const json_value_t *root, const char *expr);
json_path_result_t *json_path_query(const json_value_t *root, const char *expr);
size_t json_path_result_size(const json_path_result_t *result);
json_value_t *json_path_result_get(const json_path_result_t *result, size_t index);
void json_path_result_free(json_path_result_t *result);
const char *json_path_get_error(void);

/* ============================================================================
 * Builder API
 * ============================================================================ */

json_value_t *json_create_object(void);
json_value_t *json_create_array(void);
json_value_t *json_create_string(const char *str);
json_value_t *json_create_number(double num);
json_value_t *json_create_bool(bool val);
json_value_t *json_create_null(void);

void json_object_add(json_value_t *obj, const char *key, json_value_t *val);
void json_array_add(json_value_t *arr, json_value_t *val);

void json_object_set_string(json_value_t *obj, const char *key, const char *val);
void json_object_set_number(json_value_t *obj, const char *key, double val);
void json_object_set_bool(json_value_t *obj, const char *key, bool val);
void json_object_set_null(json_value_t *obj, const char *key);


/* ============================================================================
 * SAX/Stream API - O(1) memory, callback-based parsing
 * ============================================================================ */

typedef struct json_sax_handler_s {
  int (*on_null)(void *ctx);
  int (*on_bool)(void *ctx, bool val);
  int (*on_number)(void *ctx, double val);
  int (*on_string)(void *ctx, const char *val, size_t len);
  int (*on_object_start)(void *ctx);
  int (*on_object_key)(void *ctx, const char *key, size_t len);
  int (*on_object_end)(void *ctx);
  int (*on_array_start)(void *ctx);
  int (*on_array_end)(void *ctx);
} json_sax_handler_t;

int json_parse_sax(const char *content, size_t len, const json_sax_handler_t *handler, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* JSON_PARSER_H */
