# CoroNet module tests

# Detect other test files automatically
cmake_add_source(TEST_SOURCES DIRS ${CMAKE_CURRENT_SOURCE_DIR} PATTERNS
                 "test*.c")
# Create executables and link libraries for each test
cmake_add_test(
  SOURCES
  ${TEST_SOURCES}
  FOLDER
  "${TARGET_NAME}/test"
  LIBS
  TurboUtils::TinyTest
  ${STATIC_TARGET_NAME}
  TurboUtils::Core
  TurboNet::WebSocketParser
  $<IF:$<TARGET_EXISTS:c-ares::cares_static>,c-ares::cares_static,c-ares::cares>
  INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${WEBSOCKET_PARSER_INCLUDE_DIRS})

set(TLS_LSAN_SUPPRESSIONS
    "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/openssl_tls_handshake.lsan:print_suppressions=1"
)
set(CORONET_SHUTDOWN_TEST_TARGETS
    test_coro_server_lifecycle
    test_stream
    test_stream_pipe
    test_datagram
    test_stream_tls
    test_stream_ws
    test_coro_tls_server
    test_coro_ws_server)

set_tests_properties(${CORONET_SHUTDOWN_TEST_TARGETS} PROPERTIES LABELS
                                                                 "shutdown")
set_tests_properties(test_stream_tls PROPERTIES ENVIRONMENT
                                                "${TLS_LSAN_SUPPRESSIONS}")
set_tests_properties(test_stream_ws PROPERTIES ENVIRONMENT
                                               "${TLS_LSAN_SUPPRESSIONS}")

add_custom_target(
  coronet_shutdown_regressions
  COMMAND $<TARGET_FILE:test_coro_server_lifecycle>
  COMMAND $<TARGET_FILE:test_stream> --filter "without use-after-free"
  COMMAND $<TARGET_FILE:test_stream_pipe> --filter "without use-after-free"
  COMMAND $<TARGET_FILE:test_datagram> --filter "without use-after-free"
  COMMAND ${CMAKE_COMMAND} -E env "${TLS_LSAN_SUPPRESSIONS}"
          $<TARGET_FILE:test_stream_tls> --filter "without use-after-free"
  COMMAND ${CMAKE_COMMAND} -E env "${TLS_LSAN_SUPPRESSIONS}"
          $<TARGET_FILE:test_stream_ws> --filter "without use-after-free"
  COMMAND $<TARGET_FILE:test_coro_tls_server> --filter "without use-after-free"
  COMMAND $<TARGET_FILE:test_coro_ws_server> --filter "without use-after-free"
  DEPENDS ${CORONET_SHUTDOWN_TEST_TARGETS}
  USES_TERMINAL
  COMMENT "Run focused CoroNet shutdown close-path regressions")

set_target_properties(coronet_shutdown_regressions PROPERTIES
  FOLDER "${TARGET_NAME}/test")
