set(TARGET_NAME uri_parser)

# Generated files
turbonet_add_grammar_target(URI
  LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/uri_lexer.re)

# Library
turbonet_collect_files(SOURCE_FILES DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(${TARGET_NAME} STATIC ${SOURCE_FILES} ${URI_LEXER_GEN})

turbonet_configure_target(${TARGET_NAME}
  FOLDER "parser/${TARGET_NAME}"
  EXPORT_NAME UriParser)

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR})

# Tests
if(ENABLE_TESTS)
  add_subdirectory(test)
endif()
