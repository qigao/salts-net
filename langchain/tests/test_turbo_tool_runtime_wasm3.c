#include "tinytest.h"
#include "turbo_tool_runtime_wasm3.h"

#include <stdlib.h>

#ifndef LANGCHAIN_WASM3_TOOL_WASM_PATH
#error "LANGCHAIN_WASM3_TOOL_WASM_PATH must be defined by CMake when this test is built"
#endif

spec("turbo tool runtime wasm3") {

  it("should load a wasm3 guest tool module through the runtime") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;
    turbo_tool_runtime_tool_t tool = {0};
    char *output = NULL;
    turbo_runtime_data_bind_value_t *bind_args = NULL;
    turbo_runtime_data_bind_value_t *bind_result = NULL;

    config.module_path = LANGCHAIN_WASM3_TOOL_WASM_PATH;
    config.module_name = "langchain_tool_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_not_null(runtime);
    check_size_eq(turbo_tool_runtime_count(runtime), 1);
    check_int_eq(turbo_tool_runtime_get_tool(runtime, 0, &tool), TURBO_TOOL_OK);
    check_str_eq(tool.name, "echo_json");
    check_not_null(tool.parameters_schema);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(tool.parameters_schema, "type")),
                 "object");
    check_int_eq(
        turbo_tool_runtime_invoke(runtime, "echo_json", "{\"wasm\":true}", &output),
        TURBO_TOOL_OK);
    check_str_eq(output, "{\"wasm\":true}");

    bind_args = turbo_runtime_data_bind_value_create_object();
    check_not_null(bind_args);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     bind_args, "wasm",
                     turbo_runtime_data_bind_value_create_bool(1)),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(
        turbo_tool_runtime_invoke_bind(runtime, "echo_json", bind_args, &bind_result),
        TURBO_TOOL_OK);
    check_not_null(bind_result);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(bind_result, "wasm"), 0));

    turbo_runtime_data_bind_value_destroy(bind_result);
    turbo_runtime_data_bind_value_destroy(bind_args);
    free(output);
    turbo_tool_runtime_destroy(runtime);
  }

  it("should expose wasm3 as the default runtime backend") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;

    config.module_path = LANGCHAIN_WASM3_TOOL_WASM_PATH;
    config.module_name = "langchain_tool_guest";

    runtime = turbo_tool_runtime_default_create(&config);
    check_not_null(runtime);
    check_size_eq(turbo_tool_runtime_count(runtime), 1);

    turbo_tool_runtime_destroy(runtime);
  }
}
