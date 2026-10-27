cmake_minimum_required(VERSION 3.21)
project(SaltsNetMimeScanStandalone LANGUAGES C)
include(CTest)

add_executable(mime_scan_standalone smoke.c)
target_include_directories(mime_scan_standalone PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
set_target_properties(mime_scan_standalone PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(mime_scan_standalone PRIVATE /W4 /WX)
else()
  target_compile_options(mime_scan_standalone PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()
if(BUILD_TESTING)
  add_test(NAME mime_scan_standalone COMMAND mime_scan_standalone)
endif()
