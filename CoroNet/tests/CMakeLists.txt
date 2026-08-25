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
  OpenSSL::SSL
  ${TURBONET_CARES_TARGET}
  INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${WEBSOCKET_PARSER_INCLUDE_DIRS})

# These tests redirect only their own epoll_wait references. Production binaries
# and the CoroNet libraries continue to call libc directly.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(test_stream PRIVATE "-Wl,--wrap=epoll_wait")
  target_link_options(test_coronet_epoll_fault_logging
                      PRIVATE "-Wl,--wrap=epoll_wait")
endif()

if(TURBO_HAS_VSOCK AND TURBO_ENABLE_VSOCK_INTEGRATION_TESTS)
  target_compile_definitions(test_vsock PRIVATE
                             TURBO_VSOCK_INTEGRATION_TESTS=1)
endif()

set(TLS_LSAN_SUPPRESSIONS
    "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/openssl_tls_handshake.lsan:print_suppressions=1"
)

# Focused shutdown close-path regressions (transport close / use-after-free).
# TinyTest's --filter is an inclusion filter, so "without use-after-free"
# selects exactly the close-path cases and nothing else. test_coro_server_lifecycle
# runs unfiltered because its shutdown cases use varied names. On non-Windows the
# close-path cases in test_stream / test_stream_pipe / test_datagram are compiled
# out (#ifdef _WIN32), so those focused tests run empty and pass vacuously there.
# Run with: ctest -L shutdown
add_test(NAME test_stream.shutdown
         COMMAND test_stream --filter "without use-after-free")
add_test(NAME test_stream_pipe.shutdown
         COMMAND test_stream_pipe --filter "without use-after-free")
add_test(NAME test_datagram.shutdown
         COMMAND test_datagram --filter "without use-after-free")
add_test(NAME test_stream_tls.shutdown
         COMMAND test_stream_tls --filter "without use-after-free")
add_test(NAME test_stream_ws.shutdown
         COMMAND test_stream_ws --filter "without use-after-free")
add_test(NAME test_coro_tls_server.shutdown
         COMMAND test_coro_tls_server --filter "without use-after-free")
add_test(NAME test_coro_ws_server.shutdown
         COMMAND test_coro_ws_server --filter "without use-after-free")

set_tests_properties(
  test_coro_server_lifecycle
  test_stream.shutdown
  test_stream_pipe.shutdown
  test_datagram.shutdown
  test_stream_tls.shutdown
  test_stream_ws.shutdown
  test_coro_tls_server.shutdown
  test_coro_ws_server.shutdown
  PROPERTIES LABELS "shutdown" TIMEOUT 600)

# TLS smoke tests run under LSan in CI; apply the OpenSSL suppression file to
# both the full suite and the focused shutdown variants.
set_tests_properties(test_stream_tls PROPERTIES ENVIRONMENT
                                                "${TLS_LSAN_SUPPRESSIONS}")
set_tests_properties(test_stream_ws PROPERTIES ENVIRONMENT
                                               "${TLS_LSAN_SUPPRESSIONS}")
set_tests_properties(test_stream_tls.shutdown PROPERTIES ENVIRONMENT
                                               "${TLS_LSAN_SUPPRESSIONS}")
set_tests_properties(test_stream_ws.shutdown PROPERTIES ENVIRONMENT
                                              "${TLS_LSAN_SUPPRESSIONS}")
