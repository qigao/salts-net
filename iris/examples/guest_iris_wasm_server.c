#include <stdint.h>

__attribute__((import_module("iris"), import_name("request_param")))
extern int32_t iris_request_param(const uint8_t *name, uint32_t name_len,
                                  char *buffer, uint32_t buffer_size,
                                  uint32_t *out_written);

__attribute__((import_module("iris"), import_name("request_query")))
extern int32_t iris_request_query(const uint8_t *name, uint32_t name_len,
                                  char *buffer, uint32_t buffer_size,
                                  uint32_t *out_written);

__attribute__((import_module("iris"), import_name("request_body_read")))
extern int32_t iris_request_body_read(uint8_t *buffer, uint32_t buffer_size,
                                      uint32_t *out_read);

__attribute__((import_module("iris"), import_name("request_is_body_stream")))
extern int32_t iris_request_is_body_stream(void);

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

__attribute__((export_name("handle_hello")))
int32_t handle_hello(void) {
  static const uint8_t param_name[] = "name";
  static const uint8_t query_name[] = "lang";
  static const uint8_t content_type[] = "text/plain";
  static const uint8_t body_en[] = "hello from wasm";
  static const uint8_t body_default[] = "hello";
  char name[64];
  char lang[16];
  uint32_t written = 0;

  if (iris_request_param(param_name, 4, name, sizeof(name), &written) != 0) {
    return 1;
  }

  if (iris_request_query(query_name, 4, lang, sizeof(lang), &written) == 0 &&
      lang[0] == 'e' && lang[1] == 'n') {
    iris_response_set_status(200);
    iris_response_set_content_type(content_type, cstrlen((const char *)content_type));
    iris_response_set_body(body_en, cstrlen((const char *)body_en));
    return 0;
  }

  iris_response_set_status(200);
  iris_response_set_content_type(content_type, cstrlen((const char *)content_type));
  iris_response_set_body(body_default, cstrlen((const char *)body_default));
  return 0;
}

__attribute__((export_name("handle_echo")))
int32_t handle_echo(void) {
  static const uint8_t content_type[] = "text/plain";
  static const uint8_t ok_body[] = "stream body consumed";
  uint8_t chunk[64];
  uint32_t read_len = 0;
  uint32_t total = 0;

  if (iris_request_is_body_stream() != 1) {
    return 2;
  }

  while (1) {
    if (iris_request_body_read(chunk, sizeof(chunk), &read_len) != 0) {
      return 3;
    }
    if (read_len == 0) {
      break;
    }
    total += read_len;
  }

  if (total == 0) {
    return 4;
  }

  iris_response_set_status(200);
  iris_response_set_content_type(content_type, cstrlen((const char *)content_type));
  iris_response_set_body(ok_body, cstrlen((const char *)ok_body));
  return 0;
}
