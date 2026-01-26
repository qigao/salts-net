# Squid Examples

# Simple worker (echo server)
add_executable(squid_simple_worker
    simple_worker.c
)

target_link_libraries(squid_simple_worker
    PRIVATE
        squid
        libuv::uv
)

set_target_properties(squid_simple_worker PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    FOLDER "squid/examples"
)

# Simple master (single TCP listener)
add_executable(squid_simple_master
    simple_master.c
)

target_link_libraries(squid_simple_master
    PRIVATE
        squid
        libuv::uv
)

set_target_properties(squid_simple_master PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    FOLDER "squid/examples"
)

# Multi-listener master (TCP + TLS)
add_executable(squid_multi_listener
    multi_listener_master.c
)

target_link_libraries(squid_multi_listener
    PRIVATE
        squid
        libuv::uv
)

set_target_properties(squid_multi_listener PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    FOLDER "squid/examples"
)

# Install examples
install(TARGETS
    squid_simple_worker
    squid_simple_master
    squid_multi_listener
    RUNTIME DESTINATION bin/examples
)
