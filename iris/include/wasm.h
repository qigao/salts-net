#ifndef IRIS_WASM_H
#define IRIS_WASM_H

#include "iris_app.h"
#include "platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iris_wasm_options_s {
  uint32_t stack_size;
  size_t socket_capacity;
  const char *module_name;
  const char *handler_name;
  int stream_body;
  int enable_turbonet_host;
  int enable_http_host;
  int enable_sqlite_db;
} iris_wasm_options_t;

#define IRIS_WASM_OPTIONS_DEFAULT                                             \
  {                                                                           \
      64U * 1024U, 16U, "iris_http", "handle_request", 0, 1, 0, 0             \
  }

CXX_C_API int iris_wasm_mount(iris_app_t *app, const char *method,
                              const char *path, const char *wasm_path,
                              const iris_wasm_options_t *options);

CXX_C_API void iris_wasm_reset(void);

#ifdef __cplusplus
}
#endif

#endif
