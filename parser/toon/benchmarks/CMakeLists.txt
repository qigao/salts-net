add_executable(benchmark_toon benchmark_toon.c)
target_link_libraries(benchmark_toon PRIVATE toon)

if(UNIX)
  target_link_libraries(benchmark_toon PRIVATE m)
endif()

set_target_properties(benchmark_toon PROPERTIES FOLDER parser/toon/benchmarks)
