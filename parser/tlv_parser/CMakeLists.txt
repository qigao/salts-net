set(TARGET_NAME tlv_parser)

# =============================================================================
# Main Parser Library (respects BUILD_SHARED_LIBS)
# =============================================================================

# Find all source files
file(
    GLOB_RECURSE
    SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*
)
# Exclude memory_pool files (moved to memory module)
list(FILTER SOURCE_FILES EXCLUDE REGEX ".*memory_pool\\.(c|h)$")

# Library
add_library(${TARGET_NAME} STATIC ${SOURCE_FILES})
add_library(TurboNet::TLVParser ALIAS ${TARGET_NAME})

target_include_directories(${TARGET_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
)

# Link against dependencies
# - Memory module for allocators
# - uri_parser for backward compatibility (uri.h includes uri_parser.h)
target_link_libraries(${TARGET_NAME}
    PUBLIC
        TurboNet::Utils
        TurboNet::UriParser
)

set_target_properties(${TARGET_NAME} PROPERTIES
    FOLDER parser/tlv_parser
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME TLVParser
)
 

# =============================================================================
# Examples
# =============================================================================

add_executable(production_example
    ${CMAKE_CURRENT_SOURCE_DIR}/example/production_example.c
)
target_link_libraries(production_example PRIVATE ${TARGET_NAME})
target_include_directories(production_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
)
if(UNIX)
    target_link_libraries(production_example PRIVATE pthread)
endif()
set_target_properties(production_example PROPERTIES FOLDER parser/tlv_parser/examples)

# =============================================================================
# Tests
# =============================================================================

add_subdirectory(test)
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
