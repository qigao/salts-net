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
# Lemon Generated Files
# =============================================================================

set(MUSTACHE_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/mustache_grammar.y)
set(MUSTACHE_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/mustache_grammar_gen.c)
set(MUSTACHE_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/mustache_grammar_gen.h)

if(NOT LEMPAR)
  set(LEMPAR ${CMAKE_SOURCE_DIR}/vendor/lemon/lempar.c)
endif()

add_custom_command(
  OUTPUT ${MUSTACHE_GRAMMAR_GEN} ${MUSTACHE_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${MUSTACHE_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/mustache_grammar_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/mustache_grammar_gen.y
  DEPENDS ${MUSTACHE_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating Mustache parser with lemon"
  VERBATIM)

# =============================================================================
# Library
# =============================================================================

# Build as SHARED library (DLL)
add_library(
  mustache SHARED
  src/mustache.c
  src/mustache.h
  src/mustache_json.c
  src/mustache_json.h
  src/mustache_parser.c
  src/mustache_parser.h
  parser/mustache_lexer.h
  ${MUSTACHE_LEXER_GEN}
  ${MUSTACHE_GRAMMAR_GEN})
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
