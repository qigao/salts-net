# Netcore Clients Module
# Async client libraries for databases and services

message(STATUS "Configuring netcore clients...")

# PostgreSQL Client (pquv) 
find_package(PostgreSQL QUIET)

if(PostgreSQL_FOUND)
    add_subdirectory(postgresql)
    message(STATUS "  ✓ PostgreSQL client (pquv)")
else()
    message(STATUS "  ✗ PostgreSQL client (pquv) - libpq not found")
endif()

message(STATUS "Netcore clients configured")
