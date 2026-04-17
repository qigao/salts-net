add_executable(benchmark_tlv_parser benchmark_tlv_parser.c)
target_link_libraries(benchmark_tlv_parser PRIVATE TurboNet::TinyTest ${TARGET_NAME})
set_target_properties(benchmark_tlv_parser PROPERTIES FOLDER "parser/${TARGET_NAME}/benchmarks")
