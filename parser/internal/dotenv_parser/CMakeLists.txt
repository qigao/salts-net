set(TARGET_NAME dotenv_parser)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(DOTENV_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/src/dotenv_lexer.re)
set(DOTENV_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/dotenv_lexer_gen.c)

add_custom_command(
  OUTPUT ${DOTENV_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${DOTENV_LEXER_GEN} ${DOTENV_LEXER_RE}
  DEPENDS ${DOTENV_LEXER_RE} ${CMAKE_CURRENT_SOURCE_DIR}/src/dotenv_lexer.h
  COMMENT "Generating dotenv lexer with re2c"
  VERBATIM)

# =============================================================================
# Library
# =============================================================================

add_library(${TARGET_NAME} STATIC 
    ${DOTENV_LEXER_GEN}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dotenv.c
)
add_library(TurboNet::DotEnvParser ALIAS ${TARGET_NAME})

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE 
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/utils/include
)

target_link_libraries(${TARGET_NAME} PUBLIC TurboNet::Utils)
 
set_target_properties(
  ${TARGET_NAME} PROPERTIES FOLDER turbo_parser/parser/${TARGET_NAME}
                             EXPORT_NAME DotEnvParser)

if(BUILD_TESTS)
  add_subdirectory(test)
endif()
