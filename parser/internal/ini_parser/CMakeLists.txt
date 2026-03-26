set(TARGET_NAME ini_parser)

# Generated files
turbonet_add_grammar_target(INI
  LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/ini_lexer.re
  GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/ini_grammar.y)

# Library
turbonet_collect_files(SOURCE_FILES DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(${TARGET_NAME} STATIC ${SOURCE_FILES} ${INI_LEXER_GEN}
                                  ${INI_GRAMMAR_GEN})

turbonet_configure_target(${TARGET_NAME}
  ALIAS TurboNet::IniParser
  FOLDER "turbo_parser/parser/${TARGET_NAME}"
  EXPORT_NAME IniParser)

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(${TARGET_NAME} PUBLIC TurboNet::Utils)

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
