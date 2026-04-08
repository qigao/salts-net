set(TARGET_NAME json_parser)

# Generated files
cmake_add_grammar(JSON
  LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/json_lexer.re
  GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/json_grammar.y)

# Library
cmake_add_source(SOURCE_FILES DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(${TARGET_NAME} STATIC ${SOURCE_FILES} ${JSON_LEXER_GEN}
                                  ${JSON_GRAMMAR_GEN})

cmake_config_target(${TARGET_NAME}
  ALIAS TurboNet::JsonParser
  FOLDER "parser/${TARGET_NAME}"
  EXPORT_NAME JsonParser)

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include> $<INSTALL_INTERFACE:include/json_parser>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(${TARGET_NAME} PUBLIC TurboNet::Utils)

add_subdirectory(test)
 