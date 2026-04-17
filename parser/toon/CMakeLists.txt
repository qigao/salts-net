set(TARGET_NAME toon)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(TOON_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/toon_lexer.re)
set(TOON_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/toon_lexer_gen.c)

add_custom_command(
  OUTPUT ${TOON_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${TOON_LEXER_GEN} ${TOON_LEXER_RE}
  DEPENDS ${TOON_LEXER_RE}
  COMMENT "Generating TOON lexer with re2c"
  VERBATIM)

# =============================================================================
# Lemon Generated Files
# =============================================================================

set(TOON_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/toon_grammar.y)
set(TOON_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/toon_grammar_gen.c)
set(TOON_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/toon_grammar_gen.h)

add_custom_command(
  OUTPUT ${TOON_GRAMMAR_GEN} ${TOON_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${TOON_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/toon_grammar_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/toon_grammar_gen.y
  DEPENDS ${TOON_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating TOON parser with lemon"
  VERBATIM)

# =============================================================================
# Library
# =============================================================================

add_library(${TARGET_NAME} STATIC toonc.c ${TOON_LEXER_GEN} ${TOON_GRAMMAR_GEN})
add_library(TurboNet::Toon ALIAS ${TARGET_NAME})

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR}
)

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER parser/${TARGET_NAME}
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME Toon)

# Link dependencies
target_link_libraries(${TARGET_NAME} PUBLIC TurboNet::Utils)
if(UNIX)
  target_link_libraries(${TARGET_NAME} PRIVATE m)
endif()

# =============================================================================
# Tests
# =============================================================================

if(ENABLE_TESTS OR BUILD_TESTS)


  add_subdirectory(tests)
endif()

if(BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# =============================================================================
# Examples
# =============================================================================

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
