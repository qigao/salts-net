# S3 C Library for Amazon S3 Compatible Cloud Storage Guided rewrite from
# S3 C++ SDK
# -----------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.10)

project(
  s3_client
  DESCRIPTION "S3 C Client SDK for S3 compatible object storage"
  VERSION 0.1.0
  LANGUAGES C)

# Dependencies
find_package(OpenSSL REQUIRED)
find_package(zlib-ng CONFIG REQUIRED)
find_package(Stb REQUIRED)

# S3 C Library Target
set(S3_SOURCES
    src/s3_client.c
    src/s3_credentials.c
    src/s3_error.c
    src/s3_http.c
    src/s3_multimap.c
    src/s3_response.c
    src/s3_signer.c
    src/s3_time.c
    src/s3_url.c
    src/s3_xml_builder.c
    src/s3_sse.c
    src/s3_bucket_config.c
    src/s3_lifecycle.c
    src/s3_notification.c
    src/s3_replication.c
    src/stb_sprintf.c)

set(S3_HEADERS
    include/s3/s3_client.h
    include/s3/s3_credentials.h
    include/s3/s3_error.h
    include/s3/s3_multimap.h
    include/s3/s3_response.h
    include/s3/s3_signer.h
    include/s3/s3_time.h
    include/s3/s3_types.h
    include/s3/s3_url.h
    include/s3/s3_xml_builder.h
    include/s3/s3_sse.h
    include/s3/s3_bucket_config.h
    include/s3/s3_lifecycle.h
    include/s3/s3_notification.h
    include/s3/s3_replication.h)

add_library(s3_client SHARED ${S3_SOURCES} ${S3_HEADERS})
target_compile_definitions(s3_client PRIVATE SHARED_CXX)

# Alias for consistent naming
add_library(TurboNet::S3 ALIAS s3_client)

target_include_directories(
  s3_client
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
  PRIVATE ${Stb_INCLUDE_DIR})

target_link_libraries(
  s3_client
  PUBLIC TurboNet::HttpClient TurboNet::Utils OpenSSL::SSL OpenSSL::Crypto
  PRIVATE cxml ini_parser turbo_parser uri_parser zlib-ng::zlib PkgConfig::STC)

set_target_properties(s3_client PROPERTIES POSITION_INDEPENDENT_CODE ON
                                              FOLDER "s3"
                                              EXPORT_NAME S3)

# Install library
install(
  TARGETS s3_client
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

# Install headers
install(
  DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/include/s3/
  DESTINATION include/s3
  FILES_MATCHING
  PATTERN "*.h")

# Tests (TinyTest BDD)
add_executable(test_s3_client examples/test_s3_client.c)
target_link_libraries(test_s3_client PRIVATE TurboNet::S3
                                                TurboNet::TinyTest)
set_target_properties(test_s3_client PROPERTIES FOLDER "s3/tests")
add_test(NAME test_s3_client COMMAND test_s3_client)

add_executable(test_cxml_s3 examples/test_cxml_s3.c)
target_link_libraries(test_cxml_s3 PRIVATE TurboNet::S3
                                              TurboNet::TinyTest)
set_target_properties(test_cxml_s3 PROPERTIES FOLDER "s3/tests")
add_test(NAME test_cxml_s3 COMMAND test_cxml_s3)
