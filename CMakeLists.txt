cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5) 

# TurboNet Project - Unified High-Performance Networking Library
project(
  TurboNet
  VERSION 1.0.0
  LANGUAGES C CXX)

# Project configuration - output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Normalize install prefix (remove trailing slashes)
get_filename_component(CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" ABSOLUTE)

# Project description
set(PROJECT_DESCRIPTION "High-performance networking library")
set(PROJECT_HOMEPAGE_URL "https://github.com/qigao/turbonet")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# Language standards - unified approach
set(CMAKE_CXX_STANDARD 17) # C++17 for compatibility
set(CMAKE_C_STANDARD 11) # C17 to ensure C11 atomic support
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Enable folder organization in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "cmake")

# Enable testing
include(CTest)
enable_testing()

include(CMakeOptions.cmake)
include(Sanitizers)

# Find common dependencies used by multiple libraries
find_package(llhttp CONFIG REQUIRED)
# Find libuv (managed by vcpkg)
find_package(libuv CONFIG REQUIRED)

find_package(kcp CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(c-ares CONFIG REQUIRED)
find_package(base64 CONFIG REQUIRED)
find_package(PkgConfig REQUIRED) 

pkg_check_modules(STC REQUIRED IMPORTED_TARGET stc)

# Additional components
add_subdirectory(tools)
include(FindTools)
add_subdirectory(vendor)
add_subdirectory(shared)
add_subdirectory(parser)
add_subdirectory(tScript)
add_subdirectory(netcore)
# add_subdirectory(db)
add_subdirectory(http)
add_subdirectory(wasm)
add_subdirectory(iris)
add_subdirectory(rpc_client)
add_subdirectory(s3)

add_subdirectory(snmp)
add_subdirectory(ldap)

add_subdirectory(mustache)
add_subdirectory(ice)
add_subdirectory(lb)
add_subdirectory(tproxy)

# Package Configuration
include(CMakePackageConfigHelpers)

# Generate the config file
configure_package_config_file(
  "${CMAKE_SOURCE_DIR}/cmake/TurboNetConfig.cmake.in"
  "${CMAKE_BINARY_DIR}/TurboNetConfig.cmake"
  INSTALL_DESTINATION "lib/cmake/TurboNet")

# Generate the version file
write_basic_package_version_file(
  "${CMAKE_BINARY_DIR}/TurboNetConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY AnyNewerVersion)

# Install the config files
install(FILES "${CMAKE_BINARY_DIR}/TurboNetConfig.cmake"
              "${CMAKE_BINARY_DIR}/TurboNetConfigVersion.cmake"
        DESTINATION "lib/cmake/TurboNet")

# Install the export set
install(
  EXPORT TurboNetTargets
  FILE TurboNetTargets.cmake
  NAMESPACE TurboNet::
  DESTINATION "lib/cmake/TurboNet")
