# Log pattern lexer using re2c

# Generate lexer from re2c
set(LOG_PATTERN_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/log_pattern_lexer.re)
set(LOG_PATTERN_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/log_pattern_lexer_gen.c)

add_custom_command(
  OUTPUT ${LOG_PATTERN_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${LOG_PATTERN_LEXER_GEN} ${LOG_PATTERN_LEXER_RE}
  DEPENDS ${LOG_PATTERN_LEXER_RE} ${CMAKE_CURRENT_SOURCE_DIR}/log_pattern_lexer.h
  COMMENT "Generating log pattern lexer with re2c"
  VERBATIM
)

# Create parser library
add_library(log_pattern_lexer STATIC
  ${LOG_PATTERN_LEXER_GEN}
)
 
target_include_directories(log_pattern_lexer
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)

# Set target properties
set_target_properties(log_pattern_lexer PROPERTIES
  FOLDER "common/parser"
  OUTPUT_NAME "log_pattern_lexer"
)

# Export for parent
set(LOG_PATTERN_LEXER_LIBRARY log_pattern_lexer PARENT_SCOPE)
set(LOG_PATTERN_LEXER_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  PARENT_SCOPE
)
