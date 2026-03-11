# TLV Bind Tests

add_executable(test_tlv_bind
    test_tlv_bind.c
)

target_link_libraries(test_tlv_bind PRIVATE
    tlv_bind
    tinytest
)

target_compile_definitions(test_tlv_bind PRIVATE
    SCHEMA_FILE="${CMAKE_CURRENT_SOURCE_DIR}/../examples/order.tlvschema"
)

set_target_properties(test_tlv_bind PROPERTIES FOLDER tbe/tlv)

# Add to CTest
add_test(NAME tlv_bind_tests COMMAND test_tlv_bind)

# Benchmark
add_executable(benchmark_tlv_bind
    benchmark_comparison.c
)

target_link_libraries(benchmark_tlv_bind PRIVATE
    tlv_bind
    tinytest
)

target_compile_definitions(benchmark_tlv_bind PRIVATE
    SCHEMA_FILE="${CMAKE_CURRENT_SOURCE_DIR}/../examples/order.tlvschema"
)

set_target_properties(benchmark_tlv_bind PROPERTIES FOLDER tbe/tlv)
