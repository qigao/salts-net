# CoroNet module tests

# Detect other test files automatically
cmake_add_source(TEST_SOURCES DIRS ${CMAKE_CURRENT_SOURCE_DIR} PATTERNS "test*.c")
# Create executables and link libraries for each test
cmake_add_test(
  SOURCES ${TEST_SOURCES}
  FOLDER "${TARGET_NAME}/test"
  LIBS TurboNet::TinyTest ${STATIC_TARGET_NAME} TurboNet::Utils
       TurboNet::WebSocketParser
       $<IF:$<TARGET_EXISTS:c-ares::cares_static>,c-ares::cares_static,c-ares::cares>
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include ${WEBSOCKET_PARSER_INCLUDE_DIRS})
