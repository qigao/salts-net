# Castle - Full-featured web application example
# Demonstrates Iris framework with PostgreSQL, authentication, and CRUD operations

cmake_minimum_required(VERSION 3.15)

# Collect all source files
file(GLOB_RECURSE CASTLE_SOURCES
    src/*.c
)

# Create executable
add_executable(castle ${CASTLE_SOURCES})

# Include directories
target_include_directories(castle PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/db
    ${CMAKE_CURRENT_SOURCE_DIR}/src/handlers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/handlers/get_handlers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/handlers/post_handlers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/handlers/del_handlers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/handlers/put_handlers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/middlewares
    ${CMAKE_CURRENT_SOURCE_DIR}/src/routers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/contexts
    ${CMAKE_CURRENT_SOURCE_DIR}/src/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/src/helpers
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendors
)

# Link libraries
target_link_libraries(castle PRIVATE
    iris
    
    NetCore
    TurboNet::PostgreSQL
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Compiler settings
target_compile_features(castle PRIVATE c_std_11)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(castle PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )
endif()

# Set output properties
set_target_properties(castle PROPERTIES
    FOLDER iris/examples
    OUTPUT_NAME "castle"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Add vendor sources that castle needs
target_sources(castle PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendors/dotenv.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendors/slugify.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendors/session.c
)

message(STATUS "Castle application configured")
