cmake_minimum_required(VERSION 3.16)
project(tui LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# TUI library sources
set(TUI_SOURCES
    src/tui_buffer.c
    src/tui_buffer_ext.c
    src/tui_terminal.c
    src/tui_input.c
    src/tui_app.c
    src/tui_widgets.c
    src/tui_layout.c
    src/tui_advanced.c
)

add_library(tui STATIC ${TUI_SOURCES})

target_include_directories(tui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Platform-specific linking
if(WIN32)
    # No extra libs needed on Windows
else()
    # Unix needs nothing extra for basic terminal ops
endif()

# Examples
add_subdirectory(examples)

set_target_properties(tui PROPERTIES FOLDER "tui")
