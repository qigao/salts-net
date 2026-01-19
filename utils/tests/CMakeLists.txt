# Common module tests

# Define list of test files
set(TEST_FILES
    test_arena_buffer.c
    test_arena.c
    test_bitmap.c
    test_dns_pref.c
    test_fmt_typed.c
    test_memory_pool.c
    test_simple_arena.c
    test_tlog.c
    test_stc.c 
    test_fmt_re2c.c
    test_arena_multithread.c
    test_mutex.c
 )

# Create executables and link libraries for each test
foreach(TEST_FILE ${TEST_FILES})
    # Get the test name by removing the .c extension
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    
    # Create executable
    add_executable(${TEST_NAME} ${TEST_FILE})
    
    # Link common libraries
    target_link_libraries(${TEST_NAME} PRIVATE TurboNet::Utils unity PkgConfig::STC)
    
    # Add Windows system libraries for tests
    if(WIN32)
        target_link_libraries(${TEST_NAME} PRIVATE psapi iphlpapi userenv ws2_32)
    endif()
    
    # Add specific libraries based on test type
    if(TEST_NAME MATCHES "dns")
        target_link_libraries(${TEST_NAME} PRIVATE libuv::uv c-ares::cares)
    endif()

    if(TEST_NAME MATCHES "fmt_re2c|fmt_typed")
        target_link_libraries(${TEST_NAME} PRIVATE fmt_lexer)
    endif()
endforeach()

# Convert test file names to target names for set_target_properties
set(TEST_TARGETS)
foreach(TEST_FILE ${TEST_FILES})
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    list(APPEND TEST_TARGETS ${TEST_NAME})
endforeach()

# Set folder properties for all test targets
set_target_properties(
    ${TEST_TARGETS}
    PROPERTIES FOLDER "utils/tests"
)