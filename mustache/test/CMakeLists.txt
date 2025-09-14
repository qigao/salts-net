# Mustache Tests

if(NOT ENABLE_TESTS)
  return()
endif()

# Common configuration
set(TEST_LIBS TurboNet::Mustache TurboNet::JsonParser)
set(TEST_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../parser ${CMAKE_CURRENT_BINARY_DIR}/..)

# Test executables
add_executable(test_mustache_json test_json_integration.c)
add_executable(test_new_parser test_new_parser.c)
add_executable(test_mustache_spec test_spec_adapted.c)
add_executable(test_spec_runner test_spec_runner.c)

# Configure all tests
foreach(test IN ITEMS test_mustache_json test_new_parser test_mustache_spec
                      test_spec_runner)
  target_link_libraries(${test} PRIVATE ${TEST_LIBS})
  target_include_directories(${test} PRIVATE ${TEST_INCLUDES})
  set_target_properties(${test} PROPERTIES FOLDER mustache/tests)
  add_test(NAME ${test} COMMAND ${test})
endforeach()

# Test labels
set_tests_properties(test_mustache_json PROPERTIES LABELS "integration")
set_tests_properties(test_new_parser PROPERTIES LABELS "parser")
set_tests_properties(test_mustache_spec PROPERTIES LABELS "specification")
set_tests_properties(test_spec_runner PROPERTIES LABELS "specification;runner")
