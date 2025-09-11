 # FindPicoQUIC.cmake
# Finds the PicoQUIC library and its dependencies.
#
# This module sets:
#   PICOQUIC_FOUND          - True if PicoQUIC is found
#   PICOQUIC_LIBRARIES      - List of PicoQUIC libraries
#   PICOQUIC_INCLUDE_DIR    - Directory containing picoquic.h
#
# Set PICOQUIC_ROOT to the PicoQUIC installation directory.

# Require PICOQUIC_ROOT
if(NOT DEFINED PICOQUIC_ROOT)
    message(FATAL_ERROR "PICOQUIC_ROOT is not defined. Please set it to your PicoQUIC installation path.")
endif()

# Find the include directory for picoquic.h
find_path(PICOQUIC_INCLUDE_DIR
    NAMES picoquic.h
    PATHS ${PICOQUIC_ROOT}/include
    NO_DEFAULT_PATH
)

# Find PicoQUIC libraries
set(PICOQUIC_LIBRARIES)
set(REQUIRED_LIBRARIES picoquic)
foreach(lib_name picoquic picohttp loglib)
    find_library(${lib_name}_LIBRARY
        NAMES ${lib_name}
        PATHS ${PICOQUIC_ROOT}/lib ${PICOQUIC_ROOT}/lib/Debug ${PICOQUIC_ROOT}/lib/Release ${PICOQUIC_ROOT}/lib64
        PATH_SUFFIXES lib lib64
        NO_DEFAULT_PATH
    )
    if(${lib_name}_LIBRARY)
        list(APPEND PICOQUIC_LIBRARIES ${${lib_name}_LIBRARY})
    elseif(lib_name IN_LIST REQUIRED_LIBRARIES)
        message(FATAL_ERROR "${lib_name} library is required but not found")
    endif()
endforeach()

# Find PicoTLS (dependency for PicoQUIC)
if(NOT DEFINED PICOTLS_ROOT)
    set(PICOTLS_ROOT ${PICOQUIC_ROOT})
endif()

find_package(PicoTLS REQUIRED)
if(PICOTLS_FOUND)
    list(APPEND PICOQUIC_LIBRARIES ${PICOTLS_LIBRARIES})
else()
    message(FATAL_ERROR "PicoTLS libraries not found. Required for PicoQUIC.")
endif()

# Ensure the main picoquic library is found
if(NOT picoquic_LIBRARY)
message(FATAL_ERROR "Required PicoQUIC library (picoquic) not found.")
endif()

# Handle standard package arguments
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
PicoQUIC
REQUIRED_VARS PICOQUIC_LIBRARIES PICOQUIC_INCLUDE_DIR
FOUND_VAR PICOQUIC_FOUND
)
if(PICOQUIC_FOUND)
message(STATUS "PicoQUIC found successfully")
else()
message(WARNING "PicoQUIC not found. Check PICOQUIC_ROOT and ensure libraries and headers are installed.")
endif()

# Create an imported target if found and not already defined
if(PICOQUIC_FOUND AND NOT TARGET picoquic::picoquic)
add_library(picoquic::picoquic INTERFACE IMPORTED)
set_target_properties(
    picoquic::picoquic
    PROPERTIES
        INTERFACE_LINK_LIBRARIES "${PICOQUIC_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${PICOQUIC_INCLUDE_DIR}"
)
message(STATUS "Created imported target picoquic::picoquic with libraries: ${PICOQUIC_LIBRARIES}")
endif()
