set(TARGET_NAME datetime_parser)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(DATETIME_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/datetime_lexer.re)
set(DATETIME_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/datetime_lexer_gen.c)

add_custom_command(
  OUTPUT ${DATETIME_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${DATETIME_LEXER_GEN} ${DATETIME_LEXER_RE}
  DEPENDS ${DATETIME_LEXER_RE}
  COMMENT "Generating Date-Time lexer with re2c"
  VERBATIM)

# =============================================================================
# Lemon Generated Files
# =============================================================================

set(DATETIME_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/datetime_grammar.y)
set(DATETIME_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/datetime_grammar_gen.c)
set(DATETIME_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/datetime_grammar_gen.h)

add_custom_command(
  OUTPUT ${DATETIME_GRAMMAR_GEN} ${DATETIME_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${DATETIME_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/datetime_grammar_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/datetime_grammar_gen.y
  DEPENDS ${DATETIME_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating Date-Time parser with lemon"
  VERBATIM)

# =============================================================================
# Library
# =============================================================================

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(${TARGET_NAME} STATIC ${SOURCE_FILES} ${DATETIME_LEXER_GEN}
                                  ${DATETIME_GRAMMAR_GEN})
add_library(TurboNet::DateTimeParser ALIAS ${TARGET_NAME})

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
             EXPORT_NAME DatetimeParser)

add_subdirectory(test)
