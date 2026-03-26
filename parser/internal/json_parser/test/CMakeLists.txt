# JSON Parser tests and benchmarks
set(TEST_SOURCES test_json_parser.c benchmark_json_parser.c benchmark_query.c)

turbonet_add_tests(
    SOURCES ${TEST_SOURCES}
    FOLDER "turbo_parser/parser/${TARGET_NAME}/test"
    LIBS json_parser tinytest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
