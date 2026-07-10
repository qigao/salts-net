cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 OLD)
endif()
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

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

if(MSVC)
  add_compile_options(/experimental:c11atomics)
endif()
# Enable folder organization in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "cmake")

# Enable testing
include(CTest)
enable_testing()

include(CMakeOptions.cmake)
include(Sanitizers)
include(CmakeUtils)

find_package(kcp CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(c-ares CONFIG REQUIRED)
find_package(base64 CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(TurboUtils REQUIRED PATHS ${TURBO_UTILS_ROOT})

# Additional components
add_subdirectory(tools)
include(FindTools)
add_subdirectory(vendor)

add_subdirectory(CoroNet)
add_subdirectory(snmp)
add_subdirectory(ldap)
add_subdirectory(email)
add_subdirectory(redis)

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
export(
  EXPORT TurboNetTargets
  FILE "${CMAKE_BINARY_DIR}/TurboNetTargets.cmake"
  NAMESPACE TurboNet::)

install(
  EXPORT TurboNetTargets
  FILE TurboNetTargets.cmake
  NAMESPACE TurboNet::
  DESTINATION "lib/cmake/TurboNet")
