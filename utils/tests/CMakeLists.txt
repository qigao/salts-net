# Common module tests

# Collect test files
cmake_add_source(TEST_SOURCES DIRS ${CMAKE_CURRENT_SOURCE_DIR} PATTERNS "test_*.c" "bench_*.c")
cmake_add_source(CXX_TEST_SOURCES DIRS ${CMAKE_CURRENT_SOURCE_DIR} PATTERNS "test_*.cpp")

# Common libraries for all tests
set(COMMON_LIBS TurboNet::TinyTest TurboNet::Utils PkgConfig::STC)
if(WIN32)
  list(APPEND COMMON_LIBS psapi iphlpapi userenv ws2_32)
endif()

# Create test executables
cmake_add_test(
  SOURCES ${TEST_SOURCES} ${CXX_TEST_SOURCES}
  FOLDER "utils/tests"
  LIBS ${COMMON_LIBS} uri_parser)

# Link specific libraries for certain tests
foreach(tgt test_fmt_re2c test_fmt test_fmt_bench)
  if(TARGET ${tgt})
    target_link_libraries(${tgt} PRIVATE fmt_lexer)
  endif()
endforeach()

foreach(tgt test_turbo_str test_str_bench)
  if(TARGET ${tgt})
    target_link_libraries(${tgt} PRIVATE TurboNet::SDS)
  endif()
endforeach()
