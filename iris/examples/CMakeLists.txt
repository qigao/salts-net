# Iris Framework Examples

# Vendors - Shared libraries used by examples
add_subdirectory(vendors)

# Castle - Full-featured web application with PostgreSQL
# add_subdirectory(castle)

# JWT Example
 
# JWT Example
add_executable(jwt_example jwt_example.c)
target_link_libraries(jwt_example PRIVATE iris cjwt)
set_target_properties(jwt_example PROPERTIES FOLDER ${TARGET_NAME}/examples)

# SSE Client Example

# SSE Client Example
add_executable(test_sse_client test_sse_client.c)
target_link_libraries(test_sse_client PRIVATE rpc_client http_client_async TurboNet::Core)
set_target_properties(test_sse_client PROPERTIES FOLDER ${TARGET_NAME}/examples)

# Unified SSE/RPC Server
add_executable(unified_sse_rpc_server unified_sse_rpc_server.c)
target_link_libraries(unified_sse_rpc_server PRIVATE iris)
set_target_properties(unified_sse_rpc_server PROPERTIES FOLDER ${TARGET_NAME}/examples)
