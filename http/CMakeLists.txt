find_package(libuv CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(COOKIE_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/src/cookie_lexer.re)
set(COOKIE_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/cookie_lexer_gen.c)

add_custom_command(
  OUTPUT ${COOKIE_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${COOKIE_LEXER_GEN} ${COOKIE_LEXER_RE}
  DEPENDS ${COOKIE_LEXER_RE}
  COMMENT "Generating cookie lexer with re2c"
  VERBATIM)

# =============================================================================
# Lemon Generated Files
# =============================================================================

set(COOKIE_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/src/cookie_parser.y)
set(COOKIE_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/cookie_parser_gen.c)
set(COOKIE_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/cookie_parser_gen.h)


add_custom_command(
  OUTPUT ${COOKIE_GRAMMAR_GEN} ${COOKIE_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${COOKIE_GRAMMAR_Y}
  ${CMAKE_CURRENT_BINARY_DIR}/cookie_parser_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
  ${CMAKE_CURRENT_BINARY_DIR}/cookie_parser_gen.y
  DEPENDS ${COOKIE_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating cookie parser with lemon"
  VERBATIM)

# =============================================================================
# HTTP Client Library
# =============================================================================

set(HTTP_SOURCES
  src/http_client.c
  src/http_client_api.c
  src/http_common.c
  src/stb_sprintf.c
  src/cookie_parser.c
  ${COOKIE_LEXER_GEN}
  ${COOKIE_GRAMMAR_GEN})

set(HTTP_HEADERS
  include/http_client.h
  include/http_common.h
  include/cookie_parser.h
  include/cookie_jar.h
  ${COOKIE_GRAMMAR_H})

add_library(http_client SHARED ${HTTP_SOURCES} ${HTTP_HEADERS})
add_library(TurboNet::HttpClient ALIAS http_client)
target_compile_definitions(http_client PRIVATE SHARED_CXX HTTP_NO_DEPRECATION_WARNING)

target_include_directories(
  http_client
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include> $<INSTALL_INTERFACE:include/http>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}
    ${Stb_INCLUDE_DIR})

target_link_libraries(
  http_client
  PUBLIC
    TurboNet::Utils TurboNet::CoroNet zstd::libzstd
    $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>
  PRIVATE
    llhttp::llhttp_static cjwt sds
    mime_parser
)

set_target_properties(
  http_client
  PROPERTIES FOLDER http/http_client
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME HttpClient)

# Tests & Examples
add_subdirectory(examples)
add_subdirectory(turl)
add_subdirectory(tests)

# Installation
install(
  TARGETS http_client
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

install(FILES ${HTTP_HEADERS}
  DESTINATION include/http)
