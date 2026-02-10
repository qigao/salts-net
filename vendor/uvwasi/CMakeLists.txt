cmake_minimum_required(VERSION 3.11)
project (
    uvwasi
    DESCRIPTION "WASI syscall API built atop libuv"
    VERSION 0.0.23
    LANGUAGES C
)

# Point CMake at any custom modules we may ship
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# This can be a commit hash or tag
set(LIBUV_VERSION v1.44.2)

include(CMakeDependentOption)
cmake_dependent_option(BUILD_TESTS
  "Build the unit tests when uvwasi is the root project" ON
  "CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)

if(CMAKE_C_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
  list(APPEND uvwasi_cflags -fvisibility=hidden --std=gnu89)
  add_compile_definitions(NEED_UVWASI_EXPORT)

  list(APPEND uvwasi_cflags -Wall -Wsign-compare -Wextra -Wstrict-prototypes)
  list(APPEND uvwasi_cflags -Wno-unused-parameter)
endif()

if(APPLE)
   set(CMAKE_MACOSX_RPATH ON)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND uvwasi_defines _GNU_SOURCE _POSIX_C_SOURCE=200112)
endif()

## uvwasi source code files.
set(uvwasi_sources
    src/clocks.c
    src/fd_table.c
    src/path_resolver.c
    src/poll_oneoff.c
    src/sync_helpers.c
    src/uv_mapping.c
    src/uvwasi.c
    src/wasi_rights.c
    src/wasi_serdes.c
)

option(UVWASI_DEBUG_LOG "Enable debug logging" OFF)
if(UVWASI_DEBUG_LOG)
    list(APPEND uvwasi_cflags -DUVWASI_DEBUG_LOG)
endif()

# Code Coverage Configuration
add_library(coverage_config INTERFACE)

option(CODE_COVERAGE "Enable coverage reporting" OFF)
if(CODE_COVERAGE AND CMAKE_C_COMPILER_ID MATCHES "AppleClang|GNU|Clang")
    # Add required flags (GCC & LLVM/Clang)
    target_compile_options(coverage_config INTERFACE
        -O0        # no optimization
        -g         # generate debug info
        --coverage # sets all required flags
    )
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.13)
        target_link_options(coverage_config INTERFACE --coverage)
    else()
        target_link_libraries(coverage_config INTERFACE --coverage)
    endif()
endif()

# ASAN Support
option(ASAN "Enable code asan" OFF)
if(ASAN AND CMAKE_C_COMPILER_ID MATCHES "AppleClang|GNU|Clang")
    set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
    set (CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
endif()

## Static library target.
add_library(uvwasi_a STATIC ${uvwasi_sources})
# Remove OUTPUT_NAME to avoid collision with shared library on Windows
# set_target_properties(uvwasi_a PROPERTIES OUTPUT_NAME "uvwasi")
target_compile_definitions(uvwasi_a PRIVATE ${uvwasi_defines})
target_compile_options(uvwasi_a PRIVATE ${uvwasi_cflags})
set_target_properties(uvwasi_a PROPERTIES FOLDER "vendor/uvwasi")
target_include_directories(uvwasi_a PUBLIC 
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
if(CODE_COVERAGE)
    target_link_libraries(uvwasi_a PUBLIC libuv::uv TurboNet::Utils)
else()
    target_link_libraries(uvwasi_a PUBLIC libuv::uv TurboNet::Utils)
endif()

## Shared library target.
add_library(uvwasi SHARED ${uvwasi_sources})
target_compile_definitions(uvwasi PRIVATE ${uvwasi_defines})
target_compile_options(uvwasi PRIVATE ${uvwasi_cflags})
set_target_properties(uvwasi PROPERTIES FOLDER "vendor/uvwasi")
target_include_directories(uvwasi PRIVATE 
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
if(CODE_COVERAGE)
    target_link_libraries(uvwasi PUBLIC libuv::uv coverage_config TurboNet::Utils)
else()
    target_link_libraries(uvwasi PRIVATE libuv::uv TurboNet::Utils)
endif()


## Test targets.
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

message(STATUS "summary of uvwasi build options:
 
     Install prefix:  ${CMAKE_INSTALL_PREFIX}
     Target system:   ${CMAKE_SYSTEM_NAME}
     Compiler:
       C compiler:    ${CMAKE_C_COMPILER}
       CFLAGS:        ${CMAKE_C_FLAGS_${_build_type}} ${CMAKE_C_FLAGS}
 
     Debug logging:   ${UVWASI_DEBUG_LOG}
     Code coverage:   ${CODE_COVERAGE}
     ASAN:            ${ASAN}
     Build tests:     ${UVWASI_BUILD_TESTS}
")
