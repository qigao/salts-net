# Common module tests

# Define list of test files
set(TEST_FILES
    test_arena_buffer.c
    test_arena.c
    test_bitmap.c
    test_dns_pref.c
    test_memory_pool.c
    test_simple_arena.c
    test_turbo_logger.c
    test_stc.c
    test_stc_cstr.c
    test_stc_hmap_struct.c
)

# Create executables and link libraries for each test
foreach(TEST_FILE ${TEST_FILES})
    # Get the test name by removing the .c extension
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    
    # Create executable
    add_executable(${TEST_NAME} ${TEST_FILE})
    
    # Link common libraries
    target_link_libraries(${TEST_NAME} PRIVATE TurboNet::Common unity PkgConfig::STC)
    
    # Add Windows system libraries for tests
    if(WIN32)
        target_link_libraries(${TEST_NAME} PRIVATE psapi iphlpapi userenv ws2_32)
    endif()
    
    # Add specific libraries based on test type
    if(TEST_NAME MATCHES "dns")
        target_link_libraries(${TEST_NAME} PRIVATE libuv::uv c-ares::cares)
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
    PROPERTIES FOLDER "common/tests"
)