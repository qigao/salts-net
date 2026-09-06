set(TARGET_NAME uri_parser)

# Generated files
cmake_add_grammar(URI
  LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/uri_lexer.re
  FOLDER "email/${TARGET_NAME}")

# Library
cmake_add_source(SOURCE_FILES DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(${TARGET_NAME} STATIC ${SOURCE_FILES} ${URI_LEXER_GEN})

cmake_config_target(${TARGET_NAME}
  ALIAS SaltsNet::UriParser
  FOLDER "email/${TARGET_NAME}"
  EXPORT_NAME UriParser)

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR})

install(
  TARGETS ${TARGET_NAME}
  EXPORT SaltsNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include
        FILES_MATCHING PATTERN "*.h")

# Tests
if(ENABLE_TESTS)
  add_subdirectory(test)
endif()
