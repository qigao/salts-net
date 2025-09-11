set(TEST_TARGET_NAME uri_parser_test)

# Test source files
set(TEST_SOURCES 
    test_setup.c
    test_memory.c
    test_parser.c
    test_edge_cases.c
    test_runner.c
    test_net_core.c
    ${UNITY_SOURCES}
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
          unity ${TARGET_NAME}
)

# Set properties
set_target_properties(${TEST_TARGET_NAME} PROPERTIES FOLDER ${TARGET_NAME}/tests)

# Add test target
add_test(NAME ${TEST_TARGET_NAME} COMMAND ${TARGET_NAME}/test)