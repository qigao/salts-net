set(TARGET_NAME ini_parser)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(INI_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/ini_lexer.re)
set(INI_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/ini_lexer_gen.c)

add_custom_command(
  OUTPUT ${INI_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${INI_LEXER_GEN} ${INI_LEXER_RE}
  DEPENDS ${INI_LEXER_RE}
  COMMENT "Generating INI lexer with re2c"
  VERBATIM)

# =============================================================================
# Lemon Generated Files
# =============================================================================

set(INI_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/ini_grammar.y)
set(INI_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/ini_grammar_gen.c)
set(INI_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/ini_grammar_gen.h)

add_custom_command(
  OUTPUT ${INI_GRAMMAR_GEN} ${INI_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${INI_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/ini_grammar_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/ini_grammar_gen.y
  DEPENDS ${INI_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating INI parser with lemon"
  VERBATIM)

# =============================================================================
# Library (respects BUILD_SHARED_LIBS)
# =============================================================================

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(${TARGET_NAME} STATIC ${SOURCE_FILES} ${INI_LEXER_GEN}
                                  ${INI_GRAMMAR_GEN})
add_library(TurboNet::IniParser ALIAS ${TARGET_NAME})

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
             EXPORT_NAME IniParser)

# Suppress warnings for generated code
if(MSVC)
  target_compile_options(
    ${TARGET_NAME}
    PRIVATE /wd4101 # Unreferenced local variable
            /wd4102 # Unreferenced label
            /wd4244 # Conversion, possible loss of data
            /wd4267 # Conversion from size_t
            /wd4996 # Deprecated functions
  )
else()
  target_compile_options(
    ${TARGET_NAME} PRIVATE -Wno-unused-parameter -Wno-unused-function
                           -Wno-unused-variable -Wno-unused-but-set-variable)
endif()

# =============================================================================
# Tests
# =============================================================================

add_subdirectory(test)
