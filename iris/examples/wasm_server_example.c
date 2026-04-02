#include "iris.h"

#include <stdio.h>

#ifndef IRIS_WASM_SERVER_EXAMPLE_WASM_PATH
#error "IRIS_WASM_SERVER_EXAMPLE_WASM_PATH must be defined by CMake"
#endif

int main(void) {
  iris_app_t *app = iris_app_default();
  iris_wasm_options_t hello_opts = IRIS_WASM_OPTIONS_DEFAULT;
  iris_wasm_options_t echo_opts = IRIS_WASM_OPTIONS_DEFAULT;

  if (!app) {
    fputs("failed to create iris app\n", stderr);
    return 1;
  }

  hello_opts.handler_name = "handle_hello";
  if (iris_wasm_mount(app, "GET", "/hello/:name",
                      IRIS_WASM_SERVER_EXAMPLE_WASM_PATH, &hello_opts) != 0) {
    fputs("failed to mount hello wasm route\n", stderr);
    return 1;
  }

  echo_opts.handler_name = "handle_echo";
  echo_opts.stream_body = 1;
  if (iris_wasm_mount(app, "POST", "/echo",
                      IRIS_WASM_SERVER_EXAMPLE_WASM_PATH, &echo_opts) != 0) {
    fputs("failed to mount echo wasm route\n", stderr);
    return 1;
  }

  puts("wasm server example listening on http://127.0.0.1:8080");
  puts("GET  /hello/alice?lang=en");
  puts("POST /echo");
  return iris_app_run(app, 8080);
}
