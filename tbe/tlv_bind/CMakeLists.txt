# TLV Bind - Dynamic TLV codec library

cmake_minimum_required(VERSION 3.15)

# Library
add_library(tlv_bind STATIC
    tlv_bind.c
    tlv_schema_parser.c
)

target_include_directories(tlv_bind PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tbe/tbe/include
)
set_target_properties(tlv_bind PROPERTIES FOLDER tbe/tlv)

# Examples
add_subdirectory(examples)

# Tests
add_subdirectory(tests)
