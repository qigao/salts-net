cmake_add_source(BENCHMARK_SOURCES DIRS ${CMAKE_CURRENT_SOURCE_DIR}
                 PATTERNS "test_bench*.c")

cmake_add_benchmark(
  SOURCES ${BENCHMARK_SOURCES}
  FOLDER "${TARGET_NAME}/benchmarks"
  LIBS TurboUtils::TinyTest ${STATIC_TARGET_NAME} TurboUtils::Core
       TurboNet::WebSocketParser
       ${TURBONET_CARES_TARGET}
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
           ${CMAKE_CURRENT_SOURCE_DIR}/../tests
           ${WEBSOCKET_PARSER_INCLUDE_DIRS})
