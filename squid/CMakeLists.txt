# Squid - Multi-Process Load Balancer
cmake_minimum_required(VERSION 3.15)

project(
  Squid
  VERSION 1.0.0
  DESCRIPTION "Multi-process load balancer for TurboNet"
  LANGUAGES C)

# Source files
set(SQUID_SOURCES src/squid_master.c src/squid_worker.c src/squid_ipc.c
                  src/squid_shm.c)

# Header files
set(SQUID_HEADERS include/squid.h)

# Create library (respects BUILD_SHARED_LIBS option)
add_library(squid ${SQUID_SOURCES} ${SQUID_HEADERS})
add_library(TurboNet::Squid ALIAS squid)

# Define DLL export macro when building shared library
if(BUILD_SHARED_LIBS)
    target_compile_definitions(squid PRIVATE SHARED_CXX)
endif()

# Set target properties
set_target_properties(
  squid
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED ON
             C_EXTENSIONS OFF
             FOLDER "squid")
target_include_directories(
  squid
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Link dependencies
target_link_libraries(squid PUBLIC TurboNet::Core libuv::uv)

# Platform-specific settings
if(MSVC)
  target_compile_options(squid PRIVATE /std:c11 /experimental:c11atomics)
  target_compile_definitions(squid PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  # GCC/Clang C11 atomics support (no extra flags needed for modern compilers)
endif()

# Examples (optional)
if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# Tests
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# Install
install(
  TARGETS squid
  EXPORT SquidTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

install(FILES ${SQUID_HEADERS} DESTINATION include/squid)
