set(TARGET_NAME json_parser)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(JSON_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/json_lexer.re)
set(JSON_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/json_lexer_gen.c)

add_custom_command(
  OUTPUT ${JSON_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${JSON_LEXER_GEN} ${JSON_LEXER_RE}
  DEPENDS ${JSON_LEXER_RE}
  COMMENT "Generating JSON lexer with re2c"
  VERBATIM)

# =============================================================================
# Lemon Generated Files
# =============================================================================

set(JSON_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/json_grammar.y)
set(JSON_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/json_grammar_gen.c)
set(JSON_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/json_grammar_gen.h)

if(NOT LEMPAR)
  set(LEMPAR ${CMAKE_SOURCE_DIR}/vendor/lemon/lempar.c)
endif()

add_custom_command(
  OUTPUT ${JSON_GRAMMAR_GEN} ${JSON_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${JSON_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/json_grammar_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/json_grammar_gen.y
  DEPENDS ${JSON_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating JSON parser with lemon"
  VERBATIM)

# =============================================================================
# Library (respects BUILD_SHARED_LIBS)
# =============================================================================

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(${TARGET_NAME} STATIC ${SOURCE_FILES} ${JSON_LEXER_GEN}
                           ${JSON_GRAMMAR_GEN})
add_library(TurboNet::JsonParser ALIAS ${TARGET_NAME})

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
         $<INSTALL_INTERFACE:include/turbo/json_parser>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(${TARGET_NAME} PUBLIC TurboNet::Common)

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER turbo_parser/parser/${TARGET_NAME}
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME JsonParser)

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

if(ENABLE_TESTS)
  add_subdirectory(test)
endif()

# # Install targets
# install(TARGETS ${TARGET_NAME}
#         EXPORT TurboNetTargets
#         LIBRARY DESTINATION lib
#         ARCHIVE DESTINATION lib
#         RUNTIME DESTINATION bin)

# install(FILES include/json_parser.h
#         DESTINATION include/turbo/json_parser)
