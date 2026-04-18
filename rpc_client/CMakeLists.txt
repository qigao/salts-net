# RPC Client - JSON-RPC 2.0 client library Built on top of http_client for
# robust HTTP transport

# Library (respects BUILD_SHARED_LIBS)
add_library(rpc_client src/rpc_client.c src/rpc_error.c src/rpc_stats.c
                       src/rpc_retry.c)
cmake_config_target(
  rpc_client
  ALIAS TurboNet::RPCClient
  EXPORT_NAME RPCClient
  FOLDER http/rpc_client)

target_include_directories(
  rpc_client PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                    $<INSTALL_INTERFACE:include>)

# Link with dependencies http_client and parser are required dependencies
target_link_libraries(rpc_client PUBLIC http_client TurboNet::Parser TurboNet::CoroNet)

# Set folder for IDE organization
set_target_properties(rpc_client PROPERTIES OUTPUT_NAME "rpc_client")

# Installation
install(
  FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/rpc_client.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/rpc_error.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/rpc_retry.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/rpc_stats.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_rpc.h
  DESTINATION include)

# Examples
add_subdirectory(examples)

# Tests
add_subdirectory(tests)
