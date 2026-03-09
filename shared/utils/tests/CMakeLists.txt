# Common module tests

# Define list of test files
set(TEST_FILES
    test_arena_buffer.c
    test_arena.c
    test_dns_pref.c
    test_fmt.c
    test_memory_pool.c
    test_mmap.c
    test_simple_arena.c
    test_tlog.c
    test_stc.c
    test_fmt_re2c.c
    test_fmt_bench.c
    test_str_bench.c
    test_arena_multithread.c
    test_mutex.c
    test_turbo_fs.c
    test_turbo_str.c
    test_native_timer.c
    test_url_codec.c
    test_threadpool.c
    test_coro.c
    test_mmap_group.c
    test_url.c
    test_url_helpers.c
    test_turbo_ring.c
    test_bucket_priority_queue.c
    test_bucket_priority_queue_spsc.c
    test_bucket_priority_queue_mpmc.c
    test_disruptor.c
    test_turbo_atomic.c
    test_object_pool.c
    bench_tlog.c
    bench_bucket_priority_queue.c
    bench_priority_queue_comparison.c
    bench_turbo_ring.c
    bench_ring_comparison.c
    bench_disruptor.c
    )

# C++ test files
set(CXX_TEST_FILES test_tlog_cpp.cpp)

# Create executables and link libraries for each test
foreach(TEST_FILE ${TEST_FILES})
  # Get the test name by removing the .c extension
  get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)

  # Create executable
  add_executable(${TEST_NAME} ${TEST_FILE})

  # Link common libraries
  target_link_libraries(${TEST_NAME} PRIVATE TurboNet::Utils TurboNet::TinyTest
                                             uri_parser PkgConfig::STC)

  # Add Windows system libraries for tests
  if(WIN32)
    target_link_libraries(${TEST_NAME} PRIVATE psapi iphlpapi userenv ws2_32)
  endif()

  # Add specific libraries based on test type
  if(TEST_NAME MATCHES "dns")
    target_link_libraries(
      ${TEST_NAME}
      PRIVATE $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>
              c-ares::cares)
  endif()

  if(TEST_NAME MATCHES "fmt_re2c|fmt")
    target_link_libraries(${TEST_NAME} PRIVATE fmt_lexer)
  endif()

  if(TEST_NAME MATCHES "turbo_str|str_bench")
    target_link_libraries(${TEST_NAME} PRIVATE TurboNet::SDS)
  endif()
endforeach()

# Create C++ test executables
foreach(TEST_FILE ${CXX_TEST_FILES})
  get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
  add_executable(${TEST_NAME} ${TEST_FILE})
  target_link_libraries(${TEST_NAME} PRIVATE TurboNet::Utils TurboNet::TinyTest
                                             PkgConfig::STC)
  if(WIN32)
    target_link_libraries(${TEST_NAME} PRIVATE psapi iphlpapi userenv ws2_32)
  endif()
endforeach()

# Convert test file names to target names for set_target_properties
set(TEST_TARGETS)
foreach(TEST_FILE ${TEST_FILES})
  get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
  list(APPEND TEST_TARGETS ${TEST_NAME})
endforeach()

foreach(TEST_FILE ${CXX_TEST_FILES})
  get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
  list(APPEND TEST_TARGETS ${TEST_NAME})
endforeach()

# Set folder properties for all test targets
set_target_properties(${TEST_TARGETS} PROPERTIES FOLDER "shared/utils/tests")
