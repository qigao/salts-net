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
add_executable(test_lexer test_lexer.c)
add_executable(test_grammar test_grammar.c)
add_executable(test_integration test_integration.c)
# Lemon parser tests removed - using re2c + recursive descent only
# add_executable(test_lemon_parser test_lemon_parser.c)
# add_executable(test_parser_comparison test_parser_comparison.c)

# Configure all tests
foreach(test IN ITEMS test_mustache_json test_new_parser test_mustache_spec
                      test_spec_runner test_lexer test_grammar test_integration)
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
set_tests_properties(test_lexer PROPERTIES LABELS "lexer;unit")
set_tests_properties(test_grammar PROPERTIES LABELS "grammar;unit")
set_tests_properties(test_integration PROPERTIES LABELS "integration;pipeline")
# Lemon parser tests removed - using re2c + recursive descent only
# set_tests_properties(test_lemon_parser PROPERTIES LABELS "lemon;parser;unit;fallback")
# set_tests_properties(test_parser_comparison PROPERTIES LABELS "comparison;parser;fallback")
