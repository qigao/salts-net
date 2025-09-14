#include "turbo_parser.h"
#include <stdlib.h>
#include <string.h>

// Parsers headers
#include "csv_parser.h"
#include "frame_parser.h" // for TLV
#include "ini_parser.h"
#include "json_parser.h"
#include "ltv_parser.h"
#include "soa_parser.h"
#include "uri_parser.h"

/* JSON */
int turbo_parse_json(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  json_value_t *val = json_parse((const char *)data, len);
  if (!val)
    return -1;
  *(json_value_t **)out = val;
  return 0;
}

void turbo_free_json(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    json_free((json_value_t *)ptr);
  *(void **)out = NULL;
}

turbo_json_type_t turbo_json_type(const json_value_t *value) {
  return (turbo_json_type_t)json_type(value);
}

bool turbo_json_is_null(const json_value_t *value) { return json_is_null(value); }

bool turbo_json_bool(const json_value_t *value) { return json_bool(value); }

double turbo_json_number(const json_value_t *value) { return json_number(value); }

const char *turbo_json_string(const json_value_t *value) { return json_string(value); }

size_t turbo_json_string_len(const json_value_t *value) { return json_string_len(value); }

size_t turbo_json_object_size(const json_value_t *obj) { return json_object_size(obj); }

const char *turbo_json_object_key(const json_value_t *obj, size_t index) {
  return json_object_key(obj, index);
}

json_value_t *turbo_json_object_value(const json_value_t *obj, size_t index) {
  return json_object_value(obj, index);
}

json_value_t *turbo_json_object_get(const json_value_t *obj, const char *key) {
  return json_object_get(obj, key);
}

size_t turbo_json_array_size(const json_value_t *arr) { return json_array_size(arr); }

json_value_t *turbo_json_array_get(const json_value_t *arr, size_t index) {
  return json_array_get(arr, index);
}

int turbo_json_get_int(const json_value_t *obj, const char *key, int def) {
  return json_get_int(obj, key, def);
}

bool turbo_json_get_bool(const json_value_t *obj, const char *key, bool def) {
  return json_get_bool(obj, key, def);
}

double turbo_json_get_double(const json_value_t *obj, const char *key, double def) {
  return json_get_double(obj, key, def);
}

const char *turbo_json_get_string(const json_value_t *obj, const char *key) {
  return json_get_string(obj, key);
}

char *turbo_json_serialize(const json_value_t *value, size_t *out_len) {
  return json_serialize(value, out_len);
}

void turbo_json_serialize_free(char *str) { json_serialize_free(str); }

/* CSV */
int turbo_parse_csv(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  csv_doc_t *doc = csv_parse((const char *)data, len);
  if (!doc)
    return -1;
  *(csv_doc_t **)out = doc;
  return 0;
}

void turbo_free_csv(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    csv_free((csv_doc_t *)ptr);
  *(void **)out = NULL;
}

size_t turbo_csv_row_count(const turbo_csv_doc_t *doc) {
  return csv_row_count((const csv_doc_t *)doc);
}

size_t turbo_csv_column_count(const turbo_csv_doc_t *doc) {
  return csv_column_count((const csv_doc_t *)doc);
}

const char *turbo_csv_get(const turbo_csv_doc_t *doc, size_t row, size_t col) {
  return csv_get((const csv_doc_t *)doc, row, col);
}

int turbo_csv_get_int(const turbo_csv_doc_t *doc, size_t row, size_t col, int def) {
  return csv_get_int((const csv_doc_t *)doc, row, col, def);
}

double turbo_csv_get_double(const turbo_csv_doc_t *doc, size_t row, size_t col, double def) {
  return csv_get_double((const csv_doc_t *)doc, row, col, def);
}

bool turbo_csv_get_bool(const turbo_csv_doc_t *doc, size_t row, size_t col, bool def) {
  return csv_get_bool((const csv_doc_t *)doc, row, col, def);
}

/* INI */
int turbo_parse_ini(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  ini_t *ini = ini_parse((const char *)data, len);
  if (!ini)
    return -1;
  *(ini_t **)out = ini;
  return 0;
}

void turbo_free_ini(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    ini_free((ini_t *)ptr);
  *(void **)out = NULL;
}

const char *turbo_ini_get(const turbo_ini_t *ini, const char *section, const char *key) {
  return ini_get((ini_t *)ini, section, key);
}

int turbo_ini_get_int(const turbo_ini_t *ini, const char *section, const char *key, int def) {
  return ini_get_int((ini_t *)ini, section, key, def);
}

bool turbo_ini_get_bool(const turbo_ini_t *ini, const char *section, const char *key, bool def) {
  return ini_get_bool((ini_t *)ini, section, key, def);
}

double turbo_ini_get_double(const turbo_ini_t *ini, const char *section, const char *key, double def) {
  return ini_get_double((ini_t *)ini, section, key, def);
}

/* URI */
int turbo_parse_uri(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;

  // Ensure data is null-terminated for uri_parse
  char *temp = (char *)malloc(len + 1);
  if (!temp)
    return -1;
  memcpy(temp, data, len);
  temp[len] = '\0';

  uri_t *uri = (uri_t *)malloc(sizeof(uri_t));
  if (!uri) {
    free(temp);
    return -1;
  }

  if (!uri_parse(temp, uri)) {
    free(temp);
    free(uri);
    return -1;
  }

  free(temp);
  *(uri_t **)out = uri;
  return 0;
}

void turbo_free_uri(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    free(ptr);
  *(void **)out = NULL;
}

const char *turbo_uri_scheme(const uri_t *uri) { return uri ? uri->scheme : NULL; }
const char *turbo_uri_userinfo(const uri_t *uri) { return uri ? uri->userinfo : NULL; }
const char *turbo_uri_host(const uri_t *uri) { return uri ? uri->host : NULL; }
int turbo_uri_port(const uri_t *uri) { return uri ? uri->port : -1; }
const char *turbo_uri_path(const uri_t *uri) { return uri ? uri->path : NULL; }
const char *turbo_uri_query(const uri_t *uri) { return uri ? uri->query : NULL; }
const char *turbo_uri_fragment(const uri_t *uri) { return uri ? uri->fragment : NULL; }
turbo_uri_host_type_t turbo_uri_host_type(const uri_t *uri) {
  return uri ? (turbo_uri_host_type_t)uri->host_type : TURBO_URI_HOST_UNKNOWN;
}
bool turbo_uri_is_valid(const uri_t *uri) { return uri ? uri->valid : false; }

/* TLV */
int turbo_parse_tlv(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  frame_t *frame = (frame_t *)calloc(1, sizeof(frame_t));
  if (!frame)
    return -1;
  int rc = frame_parse(data, len, frame, FRAME_PARSE_FLAG_NONE);
  if (rc != FRAME_PARSE_OK) {
    free(frame);
    return rc;
  }
  *(frame_t **)out = frame;
  return 0;
}

void turbo_free_tlv(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    free(ptr);
  *(void **)out = NULL;
}

uint32_t turbo_tlv_msg_id(const turbo_tlv_frame_t *frame) {
  return frame ? ((const frame_t *)frame)->msg_id : 0;
}

uint8_t turbo_tlv_version(const turbo_tlv_frame_t *frame) {
  return frame ? ((const frame_t *)frame)->version : 0;
}

uint8_t turbo_tlv_type(const turbo_tlv_frame_t *frame) {
  return frame ? ((const frame_t *)frame)->payload_type : 0;
}

size_t turbo_tlv_payload_size(const turbo_tlv_frame_t *frame) {
  return frame ? ((const frame_t *)frame)->payload_size : 0;
}

const char *turbo_tlv_payload(const turbo_tlv_frame_t *frame) {
  return frame ? ((const frame_t *)frame)->payload : NULL;
}

uint32_t turbo_tlv_crc32(const turbo_tlv_frame_t *frame) {
  return frame ? ((const frame_t *)frame)->crc32 : 0;
}

int turbo_tlv_peek_size(const uint8_t *data, size_t len, uint32_t *out_size) {
  return (int)frame_peek_size(data, len, out_size);
}

/* LTV */
int turbo_parse_ltv(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  ltv_message_t *ltv = (ltv_message_t *)calloc(1, sizeof(ltv_message_t));
  if (!ltv)
    return -1;
  int rc = ltv_parse(data, len, ltv);
  if (rc != LTV_PARSE_OK) {
    free(ltv);
    return rc;
  }
  *(ltv_message_t **)out = ltv;
  return 0;
}

void turbo_free_ltv(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    free(ptr);
  *(void **)out = NULL;
}

uint8_t turbo_ltv_type(const turbo_ltv_message_t *msg) {
  return msg ? ((const ltv_message_t *)msg)->type : 0;
}

const uint8_t *turbo_ltv_value(const turbo_ltv_message_t *msg) {
  return msg ? ((const ltv_message_t *)msg)->value : NULL;
}

size_t turbo_ltv_value_len(const turbo_ltv_message_t *msg) {
  return msg ? ((const ltv_message_t *)msg)->value_size : 0;
}

size_t turbo_ltv_wire_size(size_t value_size) {
  return ltv_wire_size(value_size);
}

size_t turbo_ltv_build(uint8_t type, const uint8_t *value, size_t value_size,
                       uint8_t *out, size_t out_len) {
  return ltv_build(type, value, value_size, out, out_len);
}

int turbo_ltv_peek_size(const uint8_t *data, size_t len, uint32_t *out_length, size_t *out_header) {
  return (int)ltv_peek_size(data, len, out_length, out_header);
}

turbo_ltv_stream_t *turbo_ltv_stream_create(size_t buffer_size) {
  return (turbo_ltv_stream_t *)ltv_stream_create(buffer_size);
}

void turbo_ltv_stream_destroy(turbo_ltv_stream_t *stream) {
  ltv_stream_destroy((ltv_stream_t *)stream);
}

int turbo_ltv_stream_feed(turbo_ltv_stream_t *stream, const uint8_t *data, size_t len, void **out) {
  if (!stream || !out)
    return -1;
  
  ltv_message_t *msg = (ltv_message_t *)calloc(1, sizeof(ltv_message_t));
  if (!msg)
    return -1;
    
  LtvParseResult rc = ltv_stream_feed((ltv_stream_t *)stream, data, len, msg);
  
  if (rc == LTV_PARSE_OK) {
    *(ltv_message_t **)out = msg;
    return 0; /* Complete */
  } else if (rc == LTV_PARSE_NEED_MORE) {
    free(msg);
    return 1; /* Need more */
  } else {
    free(msg);
    return -1; /* Error */
  }
}

void turbo_ltv_stream_reset(turbo_ltv_stream_t *stream) {
  ltv_stream_reset((ltv_stream_t *)stream);
}

/* SOA */
int turbo_parse_soa(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  soa_batch_t *batch = (soa_batch_t *)calloc(1, sizeof(soa_batch_t));
  if (!batch)
    return -1;
  int rc = soa_parse(data, len, batch);
  if (rc != SOA_PARSE_OK) {
    free(batch);
    return rc;
  }
  *(soa_batch_t **)out = batch;
  return 0;
}

void turbo_free_soa(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    free(ptr);
  *(void **)out = NULL;
}

uint32_t turbo_soa_count(const turbo_soa_batch_t *batch) {
  return batch ? ((const soa_batch_t *)batch)->count : 0;
}

uint16_t turbo_soa_schema_id(const turbo_soa_batch_t *batch) {
  return batch ? ((const soa_batch_t *)batch)->schema_id : 0;
}

uint16_t turbo_soa_present_mask(const turbo_soa_batch_t *batch) {
  return batch ? ((const soa_batch_t *)batch)->present_mask : 0;
}

int8_t turbo_soa_get_i8(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_i8((const soa_batch_t *)b, col, row);
}

uint8_t turbo_soa_get_u8(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_u8((const soa_batch_t *)b, col, row);
}

int16_t turbo_soa_get_i16(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_i16((const soa_batch_t *)b, col, row);
}

uint16_t turbo_soa_get_u16(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_u16((const soa_batch_t *)b, col, row);
}

int32_t turbo_soa_get_i32(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_i32((const soa_batch_t *)b, col, row);
}

uint32_t turbo_soa_get_u32(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_u32((const soa_batch_t *)b, col, row);
}

int64_t turbo_soa_get_i64(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_i64((const soa_batch_t *)b, col, row);
}

uint64_t turbo_soa_get_u64(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_u64((const soa_batch_t *)b, col, row);
}

double turbo_soa_get_f64(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_f64((const soa_batch_t *)b, col, row);
}

size_t turbo_soa_wire_size(const turbo_soa_schema_t *schema, uint32_t count,
                           uint16_t present_mask) {
  return soa_wire_size((const soa_schema_t *)schema, count, present_mask);
}

size_t turbo_soa_build_header(const turbo_soa_schema_t *schema, uint32_t count,
                              uint16_t present_mask, uint8_t *out, size_t out_len) {
  return soa_build_header((const soa_schema_t *)schema, count, present_mask, out,
                          out_len);
}

uint8_t turbo_soa_type_width(int type) {
  return soa_type_width((SoaColumnType)type);
}

int turbo_soa_peek_header(const uint8_t *data, size_t len, uint32_t *out_count, uint16_t *out_schema) {
  return (int)soa_peek_header(data, len, out_count, out_schema);
}

int turbo_soa_schema_count(const turbo_soa_schema_t *schema) {
  return schema ? ((const soa_schema_t *)schema)->column_count : 0;
}

int turbo_soa_schema_column_type(const turbo_soa_schema_t *schema, int idx) {
  const soa_schema_t *s = (const soa_schema_t *)schema;
  if (!s || idx < 0 || idx >= s->column_count)
    return 0; /* UNKNOWN */
  return s->columns[idx].type;
}