# MinIO C Library for Amazon S3 Compatible Cloud Storage Guided rewrite from
# MinIO C++ SDK
# -----------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.10)

project(
  minio_client
  DESCRIPTION "MinIO C Client SDK for S3 compatible object storage"
  VERSION 0.1.0
  LANGUAGES C)

# Dependencies
find_package(OpenSSL REQUIRED)
find_package(zlib-ng CONFIG REQUIRED)
find_package(Stb REQUIRED)

# MinIO C Library Target
set(MINIO_SOURCES
    src/minio_client.c
    src/minio_credentials.c
    src/minio_error.c
    src/minio_http.c
    src/minio_multimap.c
    src/minio_response.c
    src/minio_signer.c
    src/minio_time.c
    src/minio_url.c
    src/minio_xml_builder.c
    src/minio_sse.c
    src/minio_bucket_config.c
    src/minio_lifecycle.c
    src/minio_notification.c
    src/minio_replication.c
    src/stb_sprintf.c)

set(MINIO_HEADERS
    include/minio/minio_client.h
    include/minio/minio_credentials.h
    include/minio/minio_error.h
    include/minio/minio_multimap.h
    include/minio/minio_response.h
    include/minio/minio_signer.h
    include/minio/minio_time.h
    include/minio/minio_types.h
    include/minio/minio_url.h
    include/minio/minio_xml_builder.h
    include/minio/minio_sse.h
    include/minio/minio_bucket_config.h
    include/minio/minio_lifecycle.h
    include/minio/minio_notification.h
    include/minio/minio_replication.h)

add_library(minio_client SHARED ${MINIO_SOURCES} ${MINIO_HEADERS})
target_compile_definitions(minio_client PRIVATE SHARED_CXX)

# Alias for consistent naming
add_library(TurboNet::Minio ALIAS minio_client)

target_include_directories(
  minio_client
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
  PRIVATE ${Stb_INCLUDE_DIR})

target_link_libraries(
  minio_client
  PUBLIC TurboNet::HttpClient TurboNet::Utils OpenSSL::SSL OpenSSL::Crypto
  PRIVATE cxml ini_parser turbo_parser uri_parser zlib-ng::zlib PkgConfig::STC)

set_target_properties(minio_client PROPERTIES POSITION_INDEPENDENT_CODE ON
                                              FOLDER "shared/minio")

# Tests (TinyTest BDD)
add_executable(test_minio_client examples/test_minio_client.c)
target_link_libraries(test_minio_client PRIVATE TurboNet::Minio
                                                TurboNet::TinyTest)
set_target_properties(test_minio_client PROPERTIES FOLDER "shared/minio/tests")
add_test(NAME test_minio_client COMMAND test_minio_client)
