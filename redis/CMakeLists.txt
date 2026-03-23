# Redis Client (RESP Protocol) Async Redis client implementing RESP protocol

add_library(tedis SHARED redis_client.c redis_pool.c redis_cluster.c)

# Define DLL export macro when building shared library

target_compile_definitions(tedis PRIVATE SHARED_CXX)
if(BUILD_TESTING)
  target_compile_definitions(tedis PRIVATE REDIS_TESTING)
endif()

target_include_directories(
  tedis PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

target_link_libraries(tedis PUBLIC TurboNet::CoroNet TurboNet::Utils)

# Platform-specific settings
if(WIN32)
  target_compile_definitions(tedis PRIVATE _CRT_SECURE_NO_WARNINGS
                                           WIN32_LEAN_AND_MEAN)
endif()

set_target_properties(
  tedis
  PROPERTIES FOLDER tedis
             OUTPUT_NAME "tedis"
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME Redis)

# Create alias using TurboNet namespace
add_library(TurboNet::Redis ALIAS tedis)

# Tests
enable_testing()
add_subdirectory(tests)
