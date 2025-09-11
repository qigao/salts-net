cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0144 OLD)
cmake_policy(SET CMP0167 OLD)

# TLSNet Project - Unified High-Performance Networking Library
project(TLSNet VERSION 1.0.0 LANGUAGES C CXX)

# Project configuration
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Project description
set(PROJECT_DESCRIPTION "High-performance networking library")
set(PROJECT_HOMEPAGE_URL "https://github.com/qigao/turbonet")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# Language standards - unified approach
set(CMAKE_CXX_STANDARD 17)          # C++17 for compatibility
set(CMAKE_C_STANDARD 11)            # C17 to ensure C11 atomic support
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Enable folder organization in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "cmake")

# Enable testing
include(CTest)
enable_testing()

include(ProjectOptions.cmake)

# Find common dependencies used by multiple libraries
find_package(pegtl CONFIG REQUIRED)
find_package(llhttp CONFIG REQUIRED)
find_package(libuv CONFIG REQUIRED)
find_package(kcp CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(ctre CONFIG REQUIRED)
find_package(qjs REQUIRED)
find_package(glaze REQUIRED) 
find_package(Threads REQUIRED)
find_package(c-ares CONFIG REQUIRED)
find_package(Catch2 CONFIG REQUIRED)

# Global compiler flags
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    if(TLSNET_ENABLE_SIMD)
        add_compile_options(-msse2 -msse3 -mssse3 -msse4.1 -msse4.2 -mavx -mavx2)
    endif()
elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/W4 /wd4100 /wd4127)
    if(TLSNET_ENABLE_SIMD)
        add_compile_options(/arch:AVX2)
    endif()
endif()

add_subdirectory(core)  
# Additional components
add_subdirectory(vendor)
add_subdirectory(parser)
add_subdirectory(cpp)