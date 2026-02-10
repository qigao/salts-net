cmake_minimum_required(VERSION 3.10)

project(stricks C)

set(CMAKE_C_STANDARD 11)

# Check for MSVC to set appropriate flags
if(MSVC)
    add_compile_options(/W3)
else()
    add_compile_options(-Wall -Wextra -pedantic)
endif()

add_library(stricks STATIC src/stx.c src/stx.h)
add_library(TurboNet::Stricks ALIAS stricks)

target_include_directories(stricks PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
add_subdirectory(test)

set_target_properties(stricks PROPERTIES FOLDER "vendor/stricks")