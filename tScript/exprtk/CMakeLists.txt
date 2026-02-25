set(TARGET_NAME exprtk)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(exprtk_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/exprtk_lexer.re)
set(exprtk_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/exprtk_lexer_gen.c)

add_custom_command(
  OUTPUT ${exprtk_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${exprtk_LEXER_GEN} ${exprtk_LEXER_RE}
  DEPENDS ${exprtk_LEXER_RE}
  COMMENT "Generating exprtk lexer with re2c"
  VERBATIM)

# =============================================================================
# Lemon Generated Files
# =============================================================================

set(exprtk_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/exprtk_grammar.y)
set(exprtk_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/exprtk_grammar_gen.c)
set(exprtk_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/exprtk_grammar_gen.h)

add_custom_command(
  OUTPUT ${exprtk_GRAMMAR_GEN} ${exprtk_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${exprtk_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/exprtk_grammar_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/exprtk_grammar_gen.y
  DEPENDS ${exprtk_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating exprtk parser with lemon"
  VERBATIM)

# =============================================================================
# Library (respects BUILD_SHARED_LIBS)
# =============================================================================

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(${TARGET_NAME} STATIC ${SOURCE_FILES} ${exprtk_LEXER_GEN}
                                  ${exprtk_GRAMMAR_GEN})
add_library(TurboNet::exprtkParser ALIAS ${TARGET_NAME})

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(${TARGET_NAME} PUBLIC TurboNet::Utils
PRIVATE
  TurboNet::CsvParser
  TurboNet::DateTimeParser
 )

target_include_directories(${TARGET_NAME} PRIVATE)

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER turbo_script/${TARGET_NAME}
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME exprtkParser)

add_subdirectory(test)

