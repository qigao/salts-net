# NetCore module tests

# Define list of test files
set(TEST_FILES
    test_pool_sync.c
    # Add other test files here as needed
)

# Detect other test files automatically
file(GLOB ALL_TEST_SOURCES test*.c)
foreach(src ${ALL_TEST_SOURCES})
    get_filename_component(name ${src} NAME)
    list(FIND TEST_FILES ${name} found)
    if(found EQUAL -1)
        list(APPEND TEST_FILES ${name})
    endif()
endforeach()

# Create executables and link libraries for each test
foreach(TEST_FILE ${TEST_FILES})
    # Get the test name by removing the .c extension
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    
    # Create executable
    # Special case for test_pool_sync which tests internal non-exported functions
    if(TEST_NAME STREQUAL "test_pool_sync")
        add_executable(${TEST_NAME} ${TEST_FILE} "${CMAKE_CURRENT_SOURCE_DIR}/../src/pool_sync.c")
    else()
        add_executable(${TEST_NAME} ${TEST_FILE})
    endif()
    
    # Link common libraries
    target_link_libraries(${TEST_NAME} PRIVATE 
        unity 
        ${TARGET_NAME} 
        TurboNet::Utils 
        TurboNet::WebSocketParser
        libuv::uv
    )
    
    add_sanitizers(${TEST_NAME})
    
    # Add parser include directory for WebSocket tests
    target_include_directories(${TEST_NAME} PRIVATE ${WEBSOCKET_PARSER_INCLUDE_DIRS})

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    
    # Set folder property
    set_target_properties(${TEST_NAME} PROPERTIES FOLDER "${TARGET_NAME}/test")
endforeach()
