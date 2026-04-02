# Iris Framework Examples

# Vendors - Shared libraries used by examples
add_subdirectory(vendors)

# Castle - Full-featured web application with PostgreSQL
# add_subdirectory(castle)

# JWT Example
 
# JWT Example
add_executable(jwt_example jwt_example.c)
target_link_libraries(jwt_example PRIVATE iris cjwt)
set_target_properties(jwt_example PROPERTIES FOLDER ${TARGET_NAME}/examples)

# SSE Client Example

# SSE Client Example
add_executable(test_sse_client test_sse_client.c)
target_link_libraries(test_sse_client PRIVATE rpc_client TurboNet::HttpClient TurboNet::CoroNet)
set_target_properties(test_sse_client PROPERTIES FOLDER ${TARGET_NAME}/examples)

# Unified SSE/RPC Server
add_executable(unified_sse_rpc_server unified_sse_rpc_server.c)
target_link_libraries(unified_sse_rpc_server PRIVATE iris)
set_target_properties(unified_sse_rpc_server PROPERTIES FOLDER ${TARGET_NAME}/examples)

find_program(IRIS_EXAMPLE_CLANG NAMES clang clang.exe)

if(IRIS_EXAMPLE_CLANG)
  set(IRIS_WASM_SERVER_GUEST_SOURCE
      ${CMAKE_CURRENT_SOURCE_DIR}/guest_iris_wasm_server.c)
  set(IRIS_WASM_SERVER_GUEST_WASM
      ${CMAKE_CURRENT_BINARY_DIR}/guest_iris_wasm_server.wasm)

  add_custom_command(
    OUTPUT ${IRIS_WASM_SERVER_GUEST_WASM}
    COMMAND
      ${IRIS_EXAMPLE_CLANG}
      --target=wasm32-unknown-unknown
      -O2
      -nostdlib
      -Wl,--no-entry
      -Wl,--export=handle_hello
      -Wl,--export=handle_echo
      -Wl,--export-memory
      -Wl,--initial-memory=131072
      -Wl,--max-memory=131072
      -Wl,--allow-undefined
      -o ${IRIS_WASM_SERVER_GUEST_WASM}
      ${IRIS_WASM_SERVER_GUEST_SOURCE}
    DEPENDS ${IRIS_WASM_SERVER_GUEST_SOURCE}
    VERBATIM)

  add_executable(wasm_server_example wasm_server_example.c)
  target_link_libraries(wasm_server_example PRIVATE iris)
  target_compile_definitions(
    wasm_server_example
    PRIVATE IRIS_WASM_SERVER_EXAMPLE_WASM_PATH=\"${IRIS_WASM_SERVER_GUEST_WASM}\")
  add_custom_target(iris_wasm_server_guest DEPENDS ${IRIS_WASM_SERVER_GUEST_WASM})
  add_dependencies(wasm_server_example iris_wasm_server_guest)
  set_target_properties(wasm_server_example PROPERTIES FOLDER ${TARGET_NAME}/examples)
endif()
