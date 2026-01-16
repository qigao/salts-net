# Redis Client (RESP Protocol) Async Redis client implementing RESP protocol

add_library(netcore_redis redis_client.c redis_pool.c redis_cluster.c)

# Define DLL export macro when building shared library
if(BUILD_SHARED_LIBS)
  target_compile_definitions(netcore_redis PRIVATE TURBONET_BUILDING_DLL)
endif()

target_include_directories(
  netcore_redis
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
         $<INSTALL_INTERFACE:include/turbo/db>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

target_link_libraries(netcore_redis PUBLIC TurboNet::Core)

# Platform-specific settings
if(WIN32)
  target_compile_definitions(netcore_redis PRIVATE _CRT_SECURE_NO_WARNINGS
                                                   WIN32_LEAN_AND_MEAN)
endif()

set_target_properties(
  netcore_redis
  PROPERTIES FOLDER db
             OUTPUT_NAME "netcore_redis"
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME Redis)

# Create alias using TurboNet namespace
add_library(TurboNet::Redis ALIAS netcore_redis)

# Install
# install(
#   TARGETS netcore_redis
#   EXPORT TurboNetTargets
#   LIBRARY DESTINATION lib
#   ARCHIVE DESTINATION lib
#   RUNTIME DESTINATION bin)

# install(
#   FILES turbo_redis.h
#   DESTINATION include/turbo
#   RENAME redis.h)

# install(FILES redis_client.h redis_pool.h redis_cluster.h
#         DESTINATION include/turbo/db)

# Tests
enable_testing()
add_subdirectory(tests)
