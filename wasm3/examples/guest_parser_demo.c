#include <stdint.h>

/* Parser imports */
__attribute__((import_module("TurboUtils"), import_name("parser_json_parse")))
extern int32_t turboutils_parser_json_parse(const uint8_t *data, uint32_t len,
                                          uint32_t *out_handle);

__attribute__((import_module("TurboUtils"), import_name("parser_csv_parse")))
extern int32_t turboutils_parser_csv_parse(const uint8_t *data, uint32_t len,
                                         uint32_t *out_handle);

__attribute__((import_module("TurboUtils"), import_name("parser_xml_parse")))
extern int32_t turboutils_parser_xml_parse(const uint8_t *data, uint32_t len,
                                         uint32_t *out_handle);

__attribute__((import_module("TurboUtils"), import_name("parser_free")))
extern int32_t turboutils_parser_free(uint32_t handle);

/* JSON accessors */
__attribute__((import_module("TurboUtils"), import_name("json_get_string")))
extern int32_t turboutils_json_get_string(uint32_t handle, const uint8_t *path, uint32_t path_len,
                                        char *buffer, uint32_t buffer_size, uint32_t *out_written);

__attribute__((import_module("TurboUtils"), import_name("json_get_int")))
extern int32_t turboutils_json_get_int(uint32_t handle, const uint8_t *path, uint32_t path_len,
                                     int32_t *out_value);

__attribute__((import_module("TurboUtils"), import_name("json_get_bool")))
extern int32_t turboutils_json_get_bool(uint32_t handle, const uint8_t *path, uint32_t path_len,
                                      int32_t *out_value);

__attribute__((import_module("TurboUtils"), import_name("json_array_size")))
extern int32_t turboutils_json_array_size(uint32_t handle, const uint8_t *path, uint32_t path_len,
                                        uint32_t *out_size);

/* CSV accessors */
__attribute__((import_module("TurboUtils"), import_name("csv_row_count")))
extern int32_t turboutils_csv_row_count(uint32_t handle, uint32_t *out_count);

__attribute__((import_module("TurboUtils"), import_name("csv_column_count")))
extern int32_t turboutils_csv_column_count(uint32_t handle, uint32_t *out_count);

__attribute__((import_module("TurboUtils"), import_name("csv_get_cell")))
extern int32_t turboutils_csv_get_cell(uint32_t handle, uint32_t row, uint32_t col,
                                     char *buffer, uint32_t buffer_size, uint32_t *out_written);

__attribute__((import_module("TurboUtils"), import_name("csv_find_column")))
extern int32_t turboutils_csv_find_column(uint32_t handle, const uint8_t *name, uint32_t name_len,
                                        uint32_t *out_col);

/* XML accessors */
__attribute__((import_module("TurboUtils"), import_name("xml_root_name")))
extern int32_t turboutils_xml_root_name(uint32_t handle, char *buffer, uint32_t buffer_size,
                                      uint32_t *out_written);

__attribute__((import_module("TurboUtils"), import_name("xml_get_text")))
extern int32_t turboutils_xml_get_text(uint32_t handle, const uint8_t *xpath, uint32_t xpath_len,
                                     char *buffer, uint32_t buffer_size, uint32_t *out_written);

static uint32_t cstrlen(const char *s) {
  uint32_t len = 0;
  while (s[len] != '\0') {
    ++len;
  }
  return len;
}

static int cstreq(const char *a, const char *b) {
  uint32_t i = 0;
  while (a[i] != '\0' && b[i] != '\0') {
    if (a[i] != b[i]) {
      return 0;
    }
    ++i;
  }
  return a[i] == b[i];
}

__attribute__((export_name("run_json_demo")))
int32_t run_json_demo(void) {
  static const uint8_t json_data[] =
      "{\"name\":\"academy\",\"version\":42,\"active\":true,\"scores\":[1,2,3]}";
  uint32_t handle = 0;
  int32_t int_value = 0;
  int32_t bool_value = 0;
  uint32_t array_size = 0;
  char name_buf[32];
  uint32_t name_len = 0;
  int32_t rc = 0;

  rc = turboutils_parser_json_parse(json_data, cstrlen((const char *)json_data), &handle);
  if (rc != 0) {
    return 10 + rc;
  }

  rc = turboutils_json_get_string(handle, (const uint8_t *)"name", 4, name_buf, sizeof(name_buf), &name_len);
  if (rc != 0 || name_len != 7 || !cstreq(name_buf, "academy")) {
    return 20 + rc;
  }

  rc = turboutils_json_get_int(handle, (const uint8_t *)"version", 7, &int_value);
  if (rc != 0 || int_value != 42) {
    return 30 + rc;
  }

  rc = turboutils_json_get_bool(handle, (const uint8_t *)"active", 6, &bool_value);
  if (rc != 0 || bool_value != 1) {
    return 40 + rc;
  }

  rc = turboutils_json_array_size(handle, (const uint8_t *)"scores", 6, &array_size);
  if (rc != 0 || array_size != 3) {
    return 50 + rc;
  }

  rc = turboutils_parser_free(handle);
  if (rc != 0) {
    return 60 + rc;
  }

  return 7;
}

__attribute__((export_name("run_csv_demo")))
int32_t run_csv_demo(void) {
  static const uint8_t csv_data[] = "name,age,city\nacademy,42,beijing\nbob,25,shanghai";
  uint32_t handle = 0;
  uint32_t rows = 0;
  uint32_t cols = 0;
  uint32_t col_index = 0;
  char cell_buf[32];
  uint32_t cell_len = 0;
  int32_t rc = 0;

  rc = turboutils_parser_csv_parse(csv_data, cstrlen((const char *)csv_data), &handle);
  if (rc != 0) {
    return 10 + rc;
  }

  rc = turboutils_csv_row_count(handle, &rows);
  if (rc != 0 || rows != 2) {
    return 20 + rc;
  }

  rc = turboutils_csv_column_count(handle, &cols);
  if (rc != 0 || cols != 3) {
    return 30 + rc;
  }

  rc = turboutils_csv_find_column(handle, (const uint8_t *)"name", 4, &col_index);
  if (rc != 0 || col_index != 0) {
    return 40 + rc;
  }

  rc = turboutils_csv_get_cell(handle, 0, col_index, cell_buf, sizeof(cell_buf), &cell_len);
  if (rc != 0 || cell_len != 7 || !cstreq(cell_buf, "academy")) {
    return 50 + rc;
  }

  rc = turboutils_csv_get_cell(handle, 1, col_index, cell_buf, sizeof(cell_buf), &cell_len);
  if (rc != 0 || cell_len != 3 || !cstreq(cell_buf, "bob")) {
    return 60 + rc;
  }

  rc = turboutils_parser_free(handle);
  if (rc != 0) {
    return 70 + rc;
  }

  return 7;
}

__attribute__((export_name("run_xml_demo")))
int32_t run_xml_demo(void) {
  static const uint8_t xml_data[] = "<root><name>academy</name><version>42</version></root>";
  uint32_t handle = 0;
  char name_buf[32];
  uint32_t name_len = 0;
  char text_buf[32];
  uint32_t text_len = 0;
  int32_t rc = 0;

  rc = turboutils_parser_xml_parse(xml_data, cstrlen((const char *)xml_data), &handle);
  if (rc != 0) {
    return 10 + rc;
  }

  rc = turboutils_xml_root_name(handle, name_buf, sizeof(name_buf), &name_len);
  if (rc != 0 || name_len != 4 || !cstreq(name_buf, "root")) {
    return 20 + rc;
  }

  rc = turboutils_xml_get_text(handle, (const uint8_t *)"//name", 6, text_buf, sizeof(text_buf), &text_len);
  if (rc != 0 || text_len != 7 || !cstreq(text_buf, "academy")) {
    return 30 + rc;
  }

  rc = turboutils_parser_free(handle);
  if (rc != 0) {
    return 40 + rc;
  }

  return 7;
}
