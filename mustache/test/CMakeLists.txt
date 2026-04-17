# Mustache Tests (current suite)
# Common configuration
set(TEST_LIBS TurboNet::Mustache TurboNet::JsonParser TurboNet::TinyTest)
set(TEST_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Test executables
add_executable(test_mustache_json test_json_integration.c)
add_executable(test_mustache_spec test_spec_adapted.c)
add_executable(test_spec_runner test_spec_runner.c)
add_executable(test_scope test_scope.c)
 
# Configure all tests
foreach(test IN ITEMS test_mustache_json test_mustache_spec
                      test_spec_runner test_scope)
  target_link_libraries(${test} PRIVATE ${TEST_LIBS})
  target_include_directories(${test} PRIVATE ${TEST_INCLUDES})
  set_target_properties(${test} PROPERTIES FOLDER mustache/tests)
  add_test(NAME ${test} COMMAND ${test})
endforeach()

target_compile_definitions(test_spec_runner PRIVATE SPEC_JSON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/spec")

# Test labels
set_tests_properties(test_mustache_json PROPERTIES LABELS "integration")
set_tests_properties(test_mustache_spec PROPERTIES LABELS "specification")
set_tests_properties(test_spec_runner PROPERTIES LABELS "specification;runner")
set_tests_properties(test_scope PROPERTIES LABELS "scope;unit")
 
