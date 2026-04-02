#include <stdint.h>

__attribute__((import_module("iris"), import_name("request_method")))
extern int32_t iris_request_method(char *buffer, uint32_t buffer_size,
                                   uint32_t *out_written);

__attribute__((import_module("iris"), import_name("request_path")))
extern int32_t iris_request_path(char *buffer, uint32_t buffer_size,
                                 uint32_t *out_written);

__attribute__((import_module("iris"), import_name("request_body")))
extern int32_t iris_request_body(uint8_t *buffer, uint32_t buffer_size,
                                 uint32_t *out_written);

__attribute__((import_module("iris"), import_name("request_header")))
extern int32_t iris_request_header(const uint8_t *name, uint32_t name_len,
                                   char *buffer, uint32_t buffer_size,
                                   uint32_t *out_written);

__attribute__((import_module("iris"), import_name("request_param")))
extern int32_t iris_request_param(const uint8_t *name, uint32_t name_len,
                                  char *buffer, uint32_t buffer_size,
                                  uint32_t *out_written);

__attribute__((import_module("iris"), import_name("request_query")))
extern int32_t iris_request_query(const uint8_t *name, uint32_t name_len,
                                  char *buffer, uint32_t buffer_size,
                                  uint32_t *out_written);

__attribute__((import_module("iris"), import_name("request_is_body_stream")))
extern int32_t iris_request_is_body_stream(void);

__attribute__((import_module("iris"), import_name("request_body_read")))
extern int32_t iris_request_body_read(uint8_t *buffer, uint32_t buffer_size,
                                      uint32_t *out_read);

__attribute__((import_module("iris"), import_name("response_set_status")))
extern int32_t iris_response_set_status(int32_t status);

__attribute__((import_module("iris"), import_name("response_set_content_type")))
extern int32_t iris_response_set_content_type(const uint8_t *value,
                                              uint32_t value_len);

__attribute__((import_module("iris"), import_name("response_set_body")))
extern int32_t iris_response_set_body(const uint8_t *value, uint32_t value_len);

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

__attribute__((export_name("handle_request")))
int32_t handle_request(void) {
  static const uint8_t header_name[] = "X-Test";
  static const uint8_t content_type[] = "application/json";
  static const uint8_t ok_body[] = "{\"ok\":true}";
  static const uint8_t bad_body[] = "{\"ok\":false}";
  char method[16];
  char path[32];
  uint8_t body[32];
  char header[16];
  uint32_t written = 0;
  int32_t rc = 0;

  rc = iris_request_method(method, sizeof(method), &written);
  if (rc != 0 || !cstreq(method, "POST")) {
    return 10 + rc;
  }

  rc = iris_request_path(path, sizeof(path), &written);
  if (rc != 0 || !cstreq(path, "/wasm")) {
    return 20 + rc;
  }

  rc = iris_request_body(body, sizeof(body), &written);
  if (written < sizeof(body)) {
    body[written] = '\0';
  }
  if (rc != 0 || written != 5 || !cstreq((const char *)body, "hello")) {
    return 30 + rc;
  }

  rc = iris_request_header(header_name, 6, header, sizeof(header), &written);
  if (rc != 0 || !cstreq(header, "1")) {
    return 40 + rc;
  }

  rc = iris_response_set_status(201);
  if (rc != 0) {
    return 50 + rc;
  }

  rc = iris_response_set_content_type(content_type, cstrlen((const char *)content_type));
  if (rc != 0) {
    return 60 + rc;
  }

  rc = iris_response_set_body(ok_body, cstrlen((const char *)ok_body));
  if (rc != 0) {
    return 70 + rc;
  }

  if (!cstreq(method, "POST")) {
    iris_response_set_status(500);
    iris_response_set_body(bad_body, cstrlen((const char *)bad_body));
    return 1;
  }

  return 0;
}

__attribute__((export_name("handle_param_request")))
int32_t handle_param_request(void) {
  static const uint8_t param_name[] = "id";
  static const uint8_t query_name[] = "lang";
  static const uint8_t content_type[] = "text/plain";
  static const uint8_t ok_body[] = "param-ok";
  char path[32];
  char id[16];
  char lang[16];
  uint32_t written = 0;
  int32_t rc = 0;

  rc = iris_request_path(path, sizeof(path), &written);
  if (rc != 0 || !cstreq(path, "/users/42")) {
    return 110 + rc;
  }

  rc = iris_request_param(param_name, 2, id, sizeof(id), &written);
  if (rc != 0 || !cstreq(id, "42")) {
    return 120 + rc;
  }

  rc = iris_request_query(query_name, 4, lang, sizeof(lang), &written);
  if (rc != 0 || !cstreq(lang, "en")) {
    return 130 + rc;
  }

  rc = iris_response_set_status(202);
  if (rc != 0) {
    return 140 + rc;
  }

  rc = iris_response_set_content_type(content_type, cstrlen((const char *)content_type));
  if (rc != 0) {
    return 150 + rc;
  }

  rc = iris_response_set_body(ok_body, cstrlen((const char *)ok_body));
  if (rc != 0) {
    return 160 + rc;
  }

  return 0;
}

__attribute__((export_name("handle_stream_request")))
int32_t handle_stream_request(void) {
  static const uint8_t content_type[] = "text/plain";
  static const uint8_t ok_body[] = "stream-ok";
  uint8_t chunk[8];
  uint32_t read_len = 0;
  uint32_t total = 0;
  int32_t rc = 0;

  rc = iris_request_is_body_stream();
  if (rc != 1) {
    return 210 + rc;
  }

  while (1) {
    rc = iris_request_body_read(chunk, sizeof(chunk), &read_len);
    if (rc != 0) {
      return 220 + rc;
    }
    if (read_len == 0) {
      break;
    }
    total += read_len;
  }

  if (total != 11) {
    return 230;
  }

  rc = iris_response_set_status(203);
  if (rc != 0) {
    return 240 + rc;
  }

  rc = iris_response_set_content_type(content_type, cstrlen((const char *)content_type));
  if (rc != 0) {
    return 250 + rc;
  }

  rc = iris_response_set_body(ok_body, cstrlen((const char *)ok_body));
  if (rc != 0) {
    return 260 + rc;
  }

  return 0;
}
