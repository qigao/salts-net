# FindPicoTLS.cmake
# Find the PicoTLS library and create imported targets
#
# This module defines:
#   PicoTLS_FOUND - True if PicoTLS is found
#   PicoTLS_INCLUDE_DIRS - Include directories for PicoTLS
#   PicoTLS_LIBRARIES - Libraries to link against
#   picotls::picotls - Imported target for PicoTLS
#
# Variables that affect this module:
#   PICOTLS_ROOT - Root directory of PicoTLS installation

# Allow user to specify root directory
if(PICOTLS_ROOT)
    set(_PICOTLS_SEARCH_PATHS ${PICOTLS_ROOT})
else()
    # Standard search paths
    set(_PICOTLS_SEARCH_PATHS
        /usr/local
        /usr
        /opt/local
        /opt
    )
endif()

# Find the include directory
find_path(PicoTLS_INCLUDE_DIR
    NAMES picotls.h
    PATHS ${_PICOTLS_SEARCH_PATHS}
    PATH_SUFFIXES include
    DOC "PicoTLS include directory"
)

# Find PicoTLS libraries
set(_PICOTLS_LIB_NAMES
    picotls-core
    picotls-openssl
    picotls-minicrypto
    picotls-minicrypto-deps
    picotls-bcrypt
    picotls-fusion
)

set(PicoTLS_LIBRARIES)
foreach(_lib_name ${_PICOTLS_LIB_NAMES})
    find_library(${_lib_name}_LIBRARY
        NAMES ${_lib_name}
        PATHS ${_PICOTLS_SEARCH_PATHS}
        PATH_SUFFIXES lib lib64
        DOC "PicoTLS ${_lib_name} library"
    )
    if(${_lib_name}_LIBRARY)
        list(APPEND PicoTLS_LIBRARIES ${${_lib_name}_LIBRARY})
        mark_as_advanced(${_lib_name}_LIBRARY)
    endif()
endforeach()

# Find OpenSSL dependency
find_package(OpenSSL QUIET)

# Handle standard package arguments
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PicoTLS
    REQUIRED_VARS PicoTLS_INCLUDE_DIR PicoTLS_LIBRARIES
    FOUND_VAR PicoTLS_FOUND
)

# Create imported target
if(PicoTLS_FOUND AND NOT TARGET picotls::picotls)
    add_library(picotls::picotls INTERFACE IMPORTED)

    set_target_properties(picotls::picotls PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${PicoTLS_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${PicoTLS_LIBRARIES}"
    )

    # Link OpenSSL if found
    if(OpenSSL_FOUND)
        set_property(TARGET picotls::picotls APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto
        )
    endif()

    # Add Windows-specific libraries
    if(WIN32)
        set_property(TARGET picotls::picotls APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES bcrypt
        )
    endif()
endif()

# Set standard variables for compatibility
if(PicoTLS_FOUND)
    set(PicoTLS_INCLUDE_DIRS ${PicoTLS_INCLUDE_DIR})
endif()

mark_as_advanced(PicoTLS_INCLUDE_DIR PicoTLS_LIBRARIES)
