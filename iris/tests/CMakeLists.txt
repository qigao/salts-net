# Iris module tests

set(IRIS_WASM_GUEST_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/guest_iris_wasm_route.c)
set(IRIS_WASM_TEST_WASM
    ${CMAKE_CURRENT_BINARY_DIR}/guest_iris_wasm_route.wasm)

find_program(IRIS_TEST_CLANG NAMES clang clang.exe)

if(IRIS_TEST_CLANG)
  add_custom_command(
    OUTPUT ${IRIS_WASM_TEST_WASM}
    COMMAND
      ${IRIS_TEST_CLANG}
      --target=wasm32-unknown-unknown
      -O2
      -nostdlib
      -Wl,--no-entry
      -Wl,--export=handle_request
      -Wl,--export=handle_param_request
      -Wl,--export=handle_stream_request
      -Wl,--export-memory
      -Wl,--initial-memory=131072
      -Wl,--max-memory=131072
      -Wl,--allow-undefined
      -o ${IRIS_WASM_TEST_WASM}
      ${IRIS_WASM_GUEST_SOURCE}
    DEPENDS ${IRIS_WASM_GUEST_SOURCE}
    VERBATIM)
endif()

# Test for route trie
add_executable(test_iris_route_trie test_route_trie.c)

target_link_libraries(test_iris_route_trie PRIVATE TurboNet::Iris tinytest
                                                   TurboUtils::Utils)

add_test(NAME IrisRouteTrie COMMAND test_iris_route_trie)

set_target_properties(test_iris_route_trie PROPERTIES FOLDER "iris/tests")

# Test for request parsing
add_executable(test_iris_request_parsing test_request_parsing.c)

target_link_libraries(test_iris_request_parsing PRIVATE TurboNet::Iris
                                                        TurboUtils::Utils tinytest)

add_test(NAME IrisRequestParsing COMMAND test_iris_request_parsing)

set_target_properties(test_iris_request_parsing PROPERTIES FOLDER "iris/tests")
target_include_directories(test_iris_request_parsing PUBLIC  ${Stb_INCLUDE_DIR})
# Test for middleware
add_executable(test_iris_middleware test_middleware.c)

target_link_libraries(test_iris_middleware PRIVATE TurboNet::Iris
                                                   TurboUtils::Utils tinytest)

add_test(NAME IrisMiddleware COMMAND test_iris_middleware)

set_target_properties(test_iris_middleware PROPERTIES FOLDER "iris/tests")

# Test for security module
add_executable(test_iris_security test_security.c)

target_link_libraries(test_iris_security PRIVATE TurboNet::Iris tinytest
                                                 TurboUtils::Utils)

add_test(NAME IrisSecurity COMMAND test_iris_security)

set_target_properties(test_iris_security PROPERTIES FOLDER "iris/tests")

# Test for configuration module
add_executable(test_iris_config test_config.c)

target_link_libraries(test_iris_config PRIVATE TurboNet::Iris tinytest
                                               TurboUtils::Utils)

add_test(NAME IrisConfig COMMAND test_iris_config)

set_target_properties(test_iris_config PROPERTIES FOLDER "iris/tests")

# Test for error recovery module
add_executable(test_iris_error_recovery test_error_recovery.c)

target_link_libraries(test_iris_error_recovery PRIVATE TurboNet::Iris tinytest
                                                       TurboUtils::Utils)

add_test(NAME IrisErrorRecovery COMMAND test_iris_error_recovery)

set_target_properties(test_iris_error_recovery PROPERTIES FOLDER "iris/tests")

# Test for output escaping integration
add_executable(test_iris_output_escaping test_output_escaping_integration.c)

target_link_libraries(test_iris_output_escaping PRIVATE TurboNet::Iris tinytest
                                                        TurboUtils::Utils)

add_test(NAME IrisOutputEscaping COMMAND test_iris_output_escaping)

set_target_properties(test_iris_output_escaping PROPERTIES FOLDER "iris/tests")

# Test for security context integration
add_executable(test_iris_security_context test_security_context.c)

target_link_libraries(test_iris_security_context PRIVATE TurboNet::Iris
                                                         TurboUtils::Utils tinytest)

add_test(NAME IrisSecurityContext COMMAND test_iris_security_context)

set_target_properties(test_iris_security_context PROPERTIES FOLDER "iris/tests")

# Test for RPC memory leak fix
add_executable(test_iris_rpc_memory_leak test_rpc_memory_leak.c)

target_link_libraries(test_iris_rpc_memory_leak PRIVATE TurboNet::Iris
                                                        TurboUtils::Utils tinytest)

add_test(NAME IrisRpcMemoryLeak COMMAND test_iris_rpc_memory_leak)

set_target_properties(test_iris_rpc_memory_leak PROPERTIES FOLDER "iris/tests")

# Test for connection context cleanup
add_executable(test_iris_connection_cleanup test_connection_cleanup.c)

target_link_libraries(test_iris_connection_cleanup
                      PRIVATE TurboNet::Iris TurboUtils::Utils tinytest)

add_test(NAME IrisConnectionCleanup COMMAND test_iris_connection_cleanup)

set_target_properties(test_iris_connection_cleanup PROPERTIES FOLDER
                                                              "iris/tests")

# Test for security properties (property-based tests)
add_executable(test_iris_security_properties test_security_properties.c)

target_link_libraries(test_iris_security_properties
                      PRIVATE TurboNet::Iris tinytest TurboUtils::Utils)

add_test(NAME IrisSecurityProperties COMMAND test_iris_security_properties)

set_target_properties(test_iris_security_properties PROPERTIES FOLDER
                                                               "iris/tests")

# Test for re2c-based security validators
add_executable(test_iris_security_re2c test_security_re2c.c)

target_link_libraries(test_iris_security_re2c PRIVATE TurboNet::Iris tinytest
                                                      TurboUtils::Utils)

add_test(NAME IrisSecurityRe2c COMMAND test_iris_security_re2c)

set_target_properties(test_iris_security_re2c PROPERTIES FOLDER "iris/tests")

# Test for re2c-based route parser
add_executable(test_iris_route_parser_re2c test_route_parser_re2c.c)

target_link_libraries(test_iris_route_parser_re2c
                      PRIVATE TurboNet::Iris TurboUtils::Utils tinytest)

add_test(NAME IrisRouteParserRe2c COMMAND test_iris_route_parser_re2c)

set_target_properties(test_iris_route_parser_re2c PROPERTIES FOLDER
                                                             "iris/tests")

# Test for coroutine integration
add_executable(test_iris_coro_integration test_coro_integration.c)

target_link_libraries(test_iris_coro_integration
                      PRIVATE TurboNet::Iris TurboUtils::Utils tinytest)

add_test(NAME IrisCoroIntegration COMMAND test_iris_coro_integration)

set_target_properties(test_iris_coro_integration PROPERTIES FOLDER
                                                            "iris/tests")

if(IRIS_TEST_CLANG)
  add_executable(test_iris_wasm_route test_wasm_route.c)
  target_link_libraries(test_iris_wasm_route PRIVATE TurboNet::Iris
                                                     TurboUtils::Utils tinytest)
  target_compile_definitions(
    test_iris_wasm_route
    PRIVATE IRIS_WASM_TEST_WASM_PATH=\"${IRIS_WASM_TEST_WASM}\")
  add_custom_target(iris_wasm_guest_test DEPENDS ${IRIS_WASM_TEST_WASM})
  add_dependencies(test_iris_wasm_route iris_wasm_guest_test)
  add_test(NAME IrisWasmRoute COMMAND test_iris_wasm_route)
  set_target_properties(test_iris_wasm_route PROPERTIES FOLDER "iris/tests")
endif()

# Benchmark for re2c-based security validators (optional)
if(BUILD_BENCHMARKS)
  add_executable(benchmark_iris_security_re2c benchmark_security_re2c.c)

  target_link_libraries(benchmark_iris_security_re2c PRIVATE TurboNet::Iris)

  set_target_properties(benchmark_iris_security_re2c
                        PROPERTIES FOLDER "iris/benchmarks")
endif()
