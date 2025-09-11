# WASMEDGE_FOUND          - True if wasmedge is found
# WASMEDGE_INCLUDE_DIRS   - Include directories for wasmedge
# WASMEDGE_LIBRARIES      - Libraries to link against
# wasmedge::wasmedge      - Imported target for wasmedge

# Hints:
# Set WASMEDGE_ROOT to the wasmedge installation directory to help locate it.

# Example usage:
# find_package(wasmedge REQUIRED)
# add_executable(my_program main.c)
# target_link_libraries(my_program PRIVATE wasmedge::wasmedge)

include(FindPackageHandleStandardArgs)


# Find wasmedge include directory
message(STATUS "Searching for wasmedge.h in include directories...")
find_path(WASMEDGE_INCLUDE_DIR
    NAMES wasmedge/wasmedge.h
    HINTS
        ${WASMEDGE_ROOT}/include
        $ENV{WASMEDGE_ROOT}/include
        /usr/local/include
        /usr/include

)

message(STATUS "WASMEDGE_INCLUDE_DIR result: ${WASMEDGE_INCLUDE_DIR}")
if(WASMEDGE_INCLUDE_DIR)
    message(STATUS "✓ Found wasmedge.h at: ${WASMEDGE_INCLUDE_DIR}")
else()
    message(STATUS "✗ wasmedge.h not found")
endif()
# Find wasmedge library
message(STATUS "Searching for wasmedge library...")
find_library(WASMEDGE_LIBRARY
    NAMES wasmedge libwasmedge
    HINTS
        ${WASMEDGE_ROOT}/lib
        ${WASMEDGE_ROOT}/lib64
        $ENV{WASMEDGE_ROOT}/lib
        $ENV{WASMEDGE_ROOT}/lib64
        /usr/local/lib
        /usr/local/lib64
        /usr/lib
        /usr/lib64
    PATH_SUFFIXES
        wasmedge
)

message(STATUS "WASMEDGE_LIBRARY result: ${WASMEDGE_LIBRARY}")
if(WASMEDGE_LIBRARY)
    message(STATUS "✓ Found wasmedge library at: ${WASMEDGE_LIBRARY}")
else()
    message(STATUS "✗ wasmedge library not found")
endif()
# Set wasmedge variables
if(WASMEDGE_INCLUDE_DIR AND WASMEDGE_LIBRARY)
    set(WASMEDGE_INCLUDE_DIRS ${WASMEDGE_INCLUDE_DIR})
    set(WASMEDGE_LIBRARIES ${WASMEDGE_LIBRARY})
    message(STATUS "✓ WasmEdge components found successfully!")
    message(STATUS "  Include dirs: ${WASMEDGE_INCLUDE_DIRS}")
    message(STATUS "  Libraries: ${WASMEDGE_LIBRARIES}")
else()
    message(STATUS "✗ WasmEdge components missing:")
    if(NOT WASMEDGE_INCLUDE_DIR)
        message(STATUS "  - Include directory not found")
    endif()
    if(NOT WASMEDGE_LIBRARY)
        message(STATUS "  - Library not found")
    endif()
endif()
# Handle the QUIETLY and REQUIRED arguments and set WASMEDGE_FOUND
find_package_handle_standard_args(wasmedge
    REQUIRED_VARS WASMEDGE_LIBRARY WASMEDGE_INCLUDE_DIR
)
# Create imported target if found
if(WASMEDGE_FOUND AND NOT TARGET wasmedge::wasmedge)
    add_library(wasmedge::wasmedge UNKNOWN IMPORTED)
    set_target_properties(wasmedge::wasmedge PROPERTIES
        IMPORTED_LOCATION "${WASMEDGE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${WASMEDGE_INCLUDE_DIR}"
    )

    # Handle Windows static linking by defining macros to disable dllimport
    if(WIN32 AND NOT BUILD_SHARED_LIBS)
        set_target_properties(wasmedge::wasmedge PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "WASMEDGE_CAPI_EXPORT="
        )
        message(STATUS "Applied Windows static linking configuration")
    endif()

    message(STATUS "✓ Created wasmedge::wasmedge imported target")
elseif(NOT WASMEDGE_FOUND)
    message(STATUS "✗ Cannot create wasmedge::wasmedge target - WasmEdge not found")
endif()
mark_as_advanced(WASMEDGE_INCLUDE_DIR WASMEDGE_LIBRARY)
