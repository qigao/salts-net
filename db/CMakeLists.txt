# Netcore Clients Module
# Async client libraries for databases and services

message(STATUS "Configuring netcore clients...")

# Redis Client (RESP protocol)
# Always available - no external dependencies
add_subdirectory(redis)
message(STATUS "  ✓ Redis client (RESP protocol)")

# PostgreSQL Client (pquv)
# Optional - requires libpq
find_package(PostgreSQL QUIET)

if(PostgreSQL_FOUND)
    add_subdirectory(postgresql)
    message(STATUS "  ✓ PostgreSQL client (pquv)")
else()
    message(STATUS "  ✗ PostgreSQL client (pquv) - libpq not found")
endif()

message(STATUS "Netcore clients configured")
