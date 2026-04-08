# JSON Parser tests and benchmarks
set(TEST_SOURCES test_json_parser.c benchmark_json_parser.c benchmark_query.c)

cmake_add_test(
    SOURCES ${TEST_SOURCES}
    FOLDER "parser/${TARGET_NAME}/test"
    LIBS json_parser tinytest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
