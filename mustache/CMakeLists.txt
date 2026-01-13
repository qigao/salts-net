set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DDEBUG")

# =============================================================================
# re2c Generated Files
# =============================================================================

set(MUSTACHE_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/mustache_lexer.re)
set(MUSTACHE_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/mustache_lexer_gen.c)

add_custom_command(
  OUTPUT ${MUSTACHE_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${MUSTACHE_LEXER_GEN} ${MUSTACHE_LEXER_RE}
  DEPENDS ${MUSTACHE_LEXER_RE}
  COMMENT "Generating Mustache lexer with re2c"
  VERBATIM)

# =============================================================================
# Library
# =============================================================================

# Build as SHARED library (DLL)
add_library(
  mustache SHARED
  src/mustache.c
  src/mustache_json.c
  src/mustache_parser.c
  ${MUSTACHE_LEXER_GEN})
add_library(TurboNet::Mustache ALIAS mustache)

# DLL export macro - matches pattern from other TurboNet libraries
target_compile_definitions(mustache PRIVATE SHARED_CXX)

target_include_directories(
  mustache
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
         $<INSTALL_INTERFACE:include/turbo/mustache>
  PRIVATE ${CMAKE_SOURCE_DIR}/parser/internal/json_parser/include
          ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Link to json_parser and common
target_link_libraries(mustache PRIVATE TurboNet::JsonParser)

set_target_properties(
  mustache
  PROPERTIES FOLDER mustache
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME Mustache)

# Example executable
if(ENABLE_EXAMPLES)
  add_executable(mustache_json_example examples/json_example.c)
  target_link_libraries(mustache_json_example PRIVATE TurboNet::Mustache
                                                      TurboNet::JsonParser)
  set_target_properties(mustache_json_example PROPERTIES FOLDER
                                                         mustache/examples)
endif()

# Tests - Include test directory
if(ENABLE_TESTS)
  add_subdirectory(test)
endif()

# Install targets
install(
  TARGETS mustache
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

install(FILES include/mustache.h include/mustache_json.h
              include/mustache_parser.h include/mustache_types.h
              include/mustache_lexer.h DESTINATION include/turbo/mustache)
