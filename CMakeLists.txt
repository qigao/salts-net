cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 OLD)
endif()
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

# SaltsNet Project - Network tools built on Salts
project(
  SaltsNet
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
set(PROJECT_HOMEPAGE_URL "https://github.com/qigao/salts-net")

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

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

if(NOT DEFINED ENV{SALTS_ROOT} OR "$ENV{SALTS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "Required dependency root is not defined: SALTS_ROOT")
endif()
if(NOT IS_DIRECTORY "$ENV{SALTS_ROOT}")
  message(FATAL_ERROR "SALTS_ROOT is not a directory: $ENV{SALTS_ROOT}")
endif()
file(TO_CMAKE_PATH "$ENV{SALTS_ROOT}" SALTSNET_SALTS_ROOT_PATH)
find_package(
  Salts 1.8.3 EXACT CONFIG REQUIRED
  PATHS "${SALTSNET_SALTS_ROOT_PATH}"
  NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH)
# Additional components
add_subdirectory(tools)
include(FindTools)
add_subdirectory(vendor)

if(SALTSNET_ENABLE_LSQUIC)
  add_subdirectory(lsquic)
endif()

add_subdirectory(ice)
add_subdirectory(snmp)
add_subdirectory(ldap)
add_subdirectory(email)
add_subdirectory(lb)
add_subdirectory(tproxy)

# Package Configuration
include(CMakePackageConfigHelpers)

# Generate the config file
configure_package_config_file(
  "${CMAKE_SOURCE_DIR}/cmake/SaltsNetConfig.cmake.in"
  "${CMAKE_BINARY_DIR}/SaltsNetConfig.cmake"
  INSTALL_DESTINATION "lib/cmake/SaltsNet")

# Generate the version file
write_basic_package_version_file(
  "${CMAKE_BINARY_DIR}/SaltsNetConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY AnyNewerVersion)

# Install the config files
install(FILES "${CMAKE_BINARY_DIR}/SaltsNetConfig.cmake"
              "${CMAKE_BINARY_DIR}/SaltsNetConfigVersion.cmake"
        DESTINATION "lib/cmake/SaltsNet")

# Install the export set
export(
  EXPORT SaltsNetTargets
  FILE "${CMAKE_BINARY_DIR}/SaltsNetTargets.cmake"
  NAMESPACE SaltsNet::)

install(
  EXPORT SaltsNetTargets
  FILE SaltsNetTargets.cmake
  NAMESPACE SaltsNet::
  DESTINATION "lib/cmake/SaltsNet")

if(BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/VerifyInstalledPackage.cmake.in"
    "${CMAKE_BINARY_DIR}/VerifyInstalledPackage.cmake"
    @ONLY)
  add_test(
    NAME saltsnet_installed_package
    COMMAND "${CMAKE_COMMAND}" -P
            "${CMAKE_BINARY_DIR}/VerifyInstalledPackage.cmake")
  set_tests_properties(saltsnet_installed_package
                       PROPERTIES LABELS package RUN_SERIAL TRUE)
endif()
