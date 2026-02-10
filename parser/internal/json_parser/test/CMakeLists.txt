set(TEST_NAME test_json_parser)

add_executable(${TEST_NAME}
    test_json_parser.c
)

target_link_libraries(${TEST_NAME}
    PRIVATE
        json_parser
        tinytest
)

target_include_directories(${TEST_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

set_target_properties(${TEST_NAME} PROPERTIES
    FOLDER   turbo_parser/parser/${TARGET_NAME}/
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

# Benchmark
add_executable(benchmark_json_parser
    benchmark_json_parser.c
)

target_link_libraries(benchmark_json_parser
    PRIVATE
        json_parser
)

target_include_directories(benchmark_json_parser
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

set_target_properties(benchmark_json_parser PROPERTIES
    FOLDER turbo_parser/parser/${TARGET_NAME}/
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Query Benchmark
add_executable(benchmark_query
    benchmark_query.c
)

target_link_libraries(benchmark_query
    PRIVATE
        json_parser
)

target_include_directories(benchmark_query
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

set_target_properties(benchmark_query PROPERTIES
    FOLDER turbo_parser/parser/${TARGET_NAME}/
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
