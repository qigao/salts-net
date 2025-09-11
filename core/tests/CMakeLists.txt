set(TEST_TARGET_NAME tlsnet_core_tests)

# Test source files
set(TEST_SOURCES
    test_error_handling.c
    test_transport_ops.c
    test_utility_functions.c
    test_integration_scenarios.c
    test_file_system.c
    test_setup.c
    test_runner.c 
)

# Create test executable
add_executable(${TEST_TARGET_NAME} ${TEST_SOURCES})

# Include directories
target_include_directories(${TEST_TARGET_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link libraries
target_link_libraries(${TEST_TARGET_NAME}
    PRIVATE
      ${TARGET_NAME}   unity
)

# Set properties
set_target_properties(${TEST_TARGET_NAME} PROPERTIES FOLDER ${TARGET_NAME}/test)

# Add test target
add_test(NAME ${TEST_TARGET_NAME} COMMAND ${TEST_TARGET_NAME})