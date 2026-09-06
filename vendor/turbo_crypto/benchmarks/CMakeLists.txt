cmake_add_benchmark(
  SOURCES benchmark_turbo_crypto.c
  LIBS turbo_crypto Salts::TinyTest
  FOLDER "vendor/turbo_crypto/benchmarks")
