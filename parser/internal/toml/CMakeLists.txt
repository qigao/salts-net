set(TARGET_NAME toml_parser)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(TOML_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/toml_lexer.re)
set(TOML_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/toml_lexer_gen.c)

add_custom_command(
  OUTPUT ${TOML_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${TOML_LEXER_GEN} ${TOML_LEXER_RE}
  DEPENDS ${TOML_LEXER_RE}
  COMMENT "Generating TOML lexer with re2c"
  VERBATIM)

# =============================================================================
# Lemon Generated Files
# =============================================================================

set(TOML_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/toml_grammar.y)
set(TOML_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/toml_grammar_gen.c)
set(TOML_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/toml_grammar_gen.h)

add_custom_command(
  OUTPUT ${TOML_GRAMMAR_GEN} ${TOML_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${TOML_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/toml_grammar_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/toml_grammar_gen.y
  DEPENDS ${TOML_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating TOML parser with lemon"
  VERBATIM)

# =============================================================================
# Library (respects BUILD_SHARED_LIBS)
# =============================================================================

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(${TARGET_NAME} STATIC ${SOURCE_FILES} ${TOML_LEXER_GEN}
                                  ${TOML_GRAMMAR_GEN})
add_library(TurboNet::TomlParser ALIAS ${TARGET_NAME})
 
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src 
          ${CMAKE_CURRENT_SOURCE_DIR}/parser
          ${CMAKE_CURRENT_BINARY_DIR})

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER turbo_parser/parser/${TARGET_NAME}
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME TomlParser)

# =============================================================================
# Tools / Examples (Optional)
# =============================================================================

add_executable(toml2json tools/toml2json.c)
target_link_libraries(toml2json PRIVATE TurboNet::TomlParser)
set_target_properties(
  toml2json PROPERTIES FOLDER turbo_parser/parser/${TARGET_NAME}/examples)

add_executable(toml-c-test tools/toml-c-test.c)
target_link_libraries(toml-c-test PRIVATE TurboNet::TomlParser)
set_target_properties(
  toml-c-test PROPERTIES FOLDER turbo_parser/parser/${TARGET_NAME}/examples)

add_executable(test_toml tests/test_toml.c)
target_link_libraries(test_toml PRIVATE TurboNet::TomlParser TurboNet::TinyTest)
set_target_properties(
  test_toml PROPERTIES FOLDER turbo_parser/parser/${TARGET_NAME}/tests)
