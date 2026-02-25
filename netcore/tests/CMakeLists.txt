# NetCore module tests

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

  add_executable(${TEST_NAME} ${TEST_FILE})

  # Link common libraries (static build to avoid DLL boundary issues)
  target_link_libraries(
    ${TEST_NAME}
    PRIVATE TurboNet::TinyTest ${STATIC_TARGET_NAME} TurboNet::Utils
            TurboNet::WebSocketParser
            $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>)

  add_sanitizers(${TEST_NAME})

  # Add parser include directory for WebSocket tests
  target_include_directories(${TEST_NAME}
                             PRIVATE ${WEBSOCKET_PARSER_INCLUDE_DIRS})

  add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

  # Set folder property
  set_target_properties(${TEST_NAME} PROPERTIES FOLDER "${TARGET_NAME}/test")
endforeach()
