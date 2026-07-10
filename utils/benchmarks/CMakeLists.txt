# Common module benchmarks

cmake_add_source(BENCHMARK_SOURCES DIRS ${CMAKE_CURRENT_SOURCE_DIR}
                 PATTERNS "bench_*.c" "test_*_bench.c")

set(COMMON_LIBS TurboNet::TinyTest TurboNet::Utils)
if(WIN32)
  list(APPEND COMMON_LIBS psapi iphlpapi userenv ws2_32)
endif()

cmake_add_benchmark(
  SOURCES ${BENCHMARK_SOURCES}
  FOLDER "utils/benchmarks"
  LIBS ${COMMON_LIBS} uri_parser)

foreach(tgt test_fmt_bench)
  if(TARGET ${tgt})
    target_link_libraries(${tgt} PRIVATE fmt_lexer)
  endif()
endforeach()

foreach(tgt test_str_bench)
  if(TARGET ${tgt})
    target_link_libraries(${tgt} PRIVATE TurboNet::SDS)
  endif()
endforeach()
