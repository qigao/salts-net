set(TARGET_NAME csv_parser)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(CSV_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/csv_lexer.re)
set(CSV_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/csv_lexer_gen.c)

add_custom_command(
  OUTPUT ${CSV_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${CSV_LEXER_GEN} ${CSV_LEXER_RE}
  DEPENDS ${CSV_LEXER_RE}
  COMMENT "Generating CSV lexer with re2c"
  VERBATIM)

set(DSV_EXPR_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/dsv_filter_expr_lexer.re)
set(DSV_EXPR_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/dsv_filter_expr_lexer_gen.c)

# =============================================================================
# Lemon Generated Files
# =============================================================================

set(CSV_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/csv_grammar.y)
set(CSV_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/csv_grammar_gen.c)
set(CSV_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/csv_grammar_gen.h)

add_custom_command(
  OUTPUT ${CSV_GRAMMAR_GEN} ${CSV_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${CSV_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/csv_grammar_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/csv_grammar_gen.y
  DEPENDS ${CSV_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating CSV parser with lemon"
  VERBATIM)

set(DSV_EXPR_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/dsv_filter_expr_grammar.y)
set(DSV_EXPR_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/dsv_filter_expr_grammar_gen.c)
set(DSV_EXPR_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/dsv_filter_expr_grammar_gen.h)

add_custom_command(
  OUTPUT ${DSV_EXPR_GRAMMAR_GEN} ${DSV_EXPR_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${DSV_EXPR_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/dsv_filter_expr_grammar_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/dsv_filter_expr_grammar_gen.y
  DEPENDS ${DSV_EXPR_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating DSV expression parser with lemon"
  VERBATIM)

add_custom_command(
  OUTPUT ${DSV_EXPR_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${DSV_EXPR_LEXER_GEN} ${DSV_EXPR_LEXER_RE}
  DEPENDS ${DSV_EXPR_LEXER_RE} ${DSV_EXPR_GRAMMAR_H}
  COMMENT "Generating DSV expression lexer with re2c"
  VERBATIM)

# =============================================================================
# Library (respects BUILD_SHARED_LIBS)
# =============================================================================

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(${TARGET_NAME} STATIC ${SOURCE_FILES} ${CSV_LEXER_GEN}
                                  ${CSV_GRAMMAR_GEN}
                                  ${DSV_EXPR_LEXER_GEN}
                                  ${DSV_EXPR_GRAMMAR_GEN})
add_library(TurboNet::CsvParser ALIAS ${TARGET_NAME})

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(${TARGET_NAME} PUBLIC TurboNet::Utils)

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER turbo_parser/parser/${TARGET_NAME}
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME CsvParser)

# =============================================================================
# Tests
# =============================================================================

add_subdirectory(test)
