cmake_minimum_required(VERSION 3.15)
project(mime_parser C)

# ── Include directories ────────────────────────────────────────────────
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ── Source files ───────────────────────────────────────────────────────
set(MIME_PARSER_SOURCES
  src/mime_parser.c
  src/mime_utils.c
  src/mime_content_disposition.c
  src/mime_encoded_word.c
  src/mime_rfc2231.c
  src/mime_rfc2822.c
  src/mime_smime.c
  src/mime_mhtml.c
)

set(MIME_PARSER_HEADERS
  include/mime_parser.h
  include/mime_utils.h
  include/mime_content_disposition.h
  include/mime_encoded_word.h
  include/mime_rfc2231.h
  include/mime_rfc2822.h
  include/mime_smime.h
  include/mime_mhtml.h
)

# ── Library target ─────────────────────────────────────────────────────
add_library(mime_parser STATIC ${MIME_PARSER_SOURCES} ${MIME_PARSER_HEADERS})

# Find OpenSSL for S/MIME support
find_package(OpenSSL REQUIRED)

target_link_libraries(mime_parser
  turbo_utils
  uri_parser
  OpenSSL::SSL
  OpenSSL::Crypto
  TurboUtils::TinyTest
)
target_include_directories(mime_parser
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
set_target_properties(mime_parser PROPERTIES FOLDER "parser/mime_parser")
add_subdirectory(test)
