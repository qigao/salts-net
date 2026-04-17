set(BENCHMARK_SOURCES benchmark_json_parser.c benchmark_query.c)

cmake_add_benchmark(
    SOURCES ${BENCHMARK_SOURCES}
    FOLDER "parser/${TARGET_NAME}/benchmarks"
    LIBS json_parser tinytest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
