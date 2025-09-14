# PostgreSQL Client (pquv) Async PostgreSQL client using libpq

# Find PostgreSQL
find_package(PostgreSQL REQUIRED)

add_library(netcore_postgresql pquv.c)

target_include_directories(
  netcore_postgresql
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
         $<INSTALL_INTERFACE:include/turbo/db>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include ${PostgreSQL_INCLUDE_DIRS})

target_link_libraries(
  netcore_postgresql
  PUBLIC TurboNet::Core
  PRIVATE PostgreSQL::PostgreSQL libuv::uv)

set_target_properties(
  netcore_postgresql
  PROPERTIES FOLDER db
             OUTPUT_NAME "netcore_postgresql"
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

# Create alias using TurboNet namespace
add_library(TurboNet::PostgreSQL ALIAS netcore_postgresql)

# Install
install(
  TARGETS netcore_postgresql
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin OPTIONAL)

install(
  FILES turbo_postgresql.h
  DESTINATION include/turbo
  RENAME postgresql.h)

install(FILES pquv.h DESTINATION include/turbo/db)
