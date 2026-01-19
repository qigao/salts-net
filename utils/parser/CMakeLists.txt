# Log pattern lexer using re2c

# Generate lexer from re2c
set(LOG_PATTERN_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/log_pattern_lexer.re)
set(LOG_PATTERN_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/log_pattern_lexer_gen.c)

add_custom_command(
  OUTPUT ${LOG_PATTERN_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${LOG_PATTERN_LEXER_GEN} ${LOG_PATTERN_LEXER_RE}
  DEPENDS ${LOG_PATTERN_LEXER_RE}
          ${CMAKE_CURRENT_SOURCE_DIR}/log_pattern_lexer.h
  COMMENT "Generating log pattern lexer with re2c"
  VERBATIM)

# Create parser library
add_library(log_pattern_lexer STATIC ${LOG_PATTERN_LEXER_GEN})

target_include_directories(
  log_pattern_lexer PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                           $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)

# Set target properties
set_target_properties(
  log_pattern_lexer PROPERTIES FOLDER "utils/parser" OUTPUT_NAME
                                                     "log_pattern_lexer")

# Export for parent
set(LOG_PATTERN_LEXER_LIBRARY
    log_pattern_lexer
    PARENT_SCOPE)
set(LOG_PATTERN_LEXER_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
    PARENT_SCOPE)

# ============================================================================
# fmt lexer using re2c
# ============================================================================

# Generate fmt lexer from re2c
set(FMT_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/fmt_lexer.re)
set(FMT_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/fmt_lexer_gen.c)

add_custom_command(
  OUTPUT ${FMT_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${FMT_LEXER_GEN} ${FMT_LEXER_RE}
  DEPENDS ${FMT_LEXER_RE} ${CMAKE_CURRENT_SOURCE_DIR}/fmt_lexer.h
  COMMENT "Generating fmt lexer with re2c"
  VERBATIM)

# Create fmt lexer library
add_library(fmt_lexer STATIC ${FMT_LEXER_GEN})

target_include_directories(
  fmt_lexer PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                   $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)

# Set target properties
set_target_properties(fmt_lexer PROPERTIES FOLDER "utils/parser" OUTPUT_NAME
                                                                 "fmt_lexer")

# Export for parent
set(FMT_LEXER_LIBRARY
    fmt_lexer
    PARENT_SCOPE)
set(FMT_LEXER_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
    PARENT_SCOPE)


