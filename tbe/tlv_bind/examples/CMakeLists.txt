# TLV Bind Examples

add_executable(tlv_dynamic_example
    dynamic_example.c
)

target_link_libraries(tlv_dynamic_example PRIVATE
    tlv_bind
)
set_target_properties(tlv_dynamic_example PROPERTIES FOLDER tbe/tlv)

# Pass schema file path as compile definition
target_compile_definitions(tlv_dynamic_example PRIVATE
    SCHEMA_FILE="${CMAKE_CURRENT_SOURCE_DIR}/order.tlvschema"
)
