set(TARGET_NAME uri_parser)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(URI_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/uri_lexer.re)
set(URI_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/uri_lexer_gen.c)

add_custom_command(
  OUTPUT ${URI_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${URI_LEXER_GEN} ${URI_LEXER_RE}
  DEPENDS ${URI_LEXER_RE}
  COMMENT "Generating URI lexer with re2c"
  VERBATIM)

# =============================================================================
# Library (respects BUILD_SHARED_LIBS)
# =============================================================================

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(${TARGET_NAME} STATIC ${SOURCE_FILES} ${URI_LEXER_GEN})
add_library(TurboNet::UriParser ALIAS ${TARGET_NAME})

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include/turbo>
         $<INSTALL_INTERFACE:include/turbo/common>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(${TARGET_NAME} PUBLIC TurboNet::Common)
 

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER turbo_parser/parser/${TARGET_NAME}
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME UriParser)

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

if(ENABLE_TESTS)
  add_subdirectory(test)
endif()
