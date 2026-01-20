# Iris Framework Examples

# Vendors - Shared libraries used by examples
add_subdirectory(vendors)

# Castle - Full-featured web application with PostgreSQL
add_subdirectory(castle)

# RPC Server Example
add_executable(rpc_server_example
    rpc_server_example.c 
)

target_include_directories(rpc_server_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendors
)

target_link_libraries(rpc_server_example PRIVATE
    iris
)

set_target_properties(rpc_server_example PROPERTIES 
    FOLDER ${TARGET_NAME}/examples
    OUTPUT_NAME "rpc_server_example"
)
 
