# Redis Client (RESP Protocol) Async Redis client implementing RESP protocol

set(REDIS_HEADERS redis_client.h redis_pool.h redis_cluster.h)

add_library(tedis SHARED redis_client.c redis_ext.c redis_pool.c redis_cluster.c
                         ${REDIS_HEADERS})

cmake_config_target(
  tedis
  ALIAS
  TurboNet::Redis
  FOLDER
  tedis
  EXPORT_NAME
  Redis)

target_compile_definitions(tedis PRIVATE SHARED_CXX)
if(BUILD_TESTING)
  target_compile_definitions(tedis PRIVATE REDIS_TESTING)
endif()

target_include_directories(
  tedis PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
               $<INSTALL_INTERFACE:include> $<INSTALL_INTERFACE:include/redis>)

target_link_libraries(tedis PUBLIC TurboNet::CoroNet TurboUtils::Core)

# Platform-specific settings
if(WIN32)
  target_compile_definitions(tedis PRIVATE _CRT_SECURE_NO_WARNINGS
                                           WIN32_LEAN_AND_MEAN)
endif()

# Tests
add_subdirectory(tests)

install(FILES ${REDIS_HEADERS} DESTINATION include/redis)
