find_package(zstd CONFIG REQUIRED) 

# Generated files_parser)
cmake_add_grammar(
  cookie LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/src/cookie_lexer.re GRAMMAR_Y
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cookie_parser.y)

# HTTP Client Library
set(HTTP_SOURCES
    src/http_client.c
    src/http_client_api.c
    src/http_common.c
    src/cookie_parser.c
    ${cookie_LEXER_GEN}
    ${cookie_GRAMMAR_GEN})

set(HTTP_HEADERS
    include/http_client.h include/http_common.h include/cookie_parser.h
    include/cookie_jar.h ${cookie_GRAMMAR_H})

add_library(http_client SHARED ${HTTP_SOURCES} ${HTTP_HEADERS})

cmake_config_target(
  http_client
  ALIAS
  TurboNet::HttpClient
  FOLDER
  http/http_client
  EXPORT_NAME
  HttpClient)

target_compile_definitions(http_client PRIVATE SHARED_CXX
                                               HTTP_NO_DEPRECATION_WARNING)

target_include_directories(
  http_client
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
         $<INSTALL_INTERFACE:include> $<INSTALL_INTERFACE:include/http>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}
          ${Stb_INCLUDE_DIR})

target_link_libraries(
  http_client
  PUBLIC TurboNet::Utils TurboNet::CoroNet zstd::libzstd TurboNet::Parser
  PRIVATE  llhttp::llhttp_static cjwt sds mime_parser)

# Tests & Examples
add_subdirectory(examples)
add_subdirectory(turl)
add_subdirectory(tests)

# Installation
install(FILES ${HTTP_HEADERS} DESTINATION include/http)
