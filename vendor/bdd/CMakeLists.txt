cmake_minimum_required(VERSION 3.5)
project(bdd)

# Use C17
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON) 
add_library(bdd INTERFACE)
target_include_directories(bdd INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

set(EXAMPLE_SOURCES example.c bdd-for-c.h)
add_executable(example_test ${EXAMPLE_SOURCES})

set(TEST_TREE_SOURCES test-tree.c bdd-for-c.h test-tree.h)
add_executable(test_tree_test ${TEST_TREE_SOURCES})

set(ARRAY_SOURCES array.c bdd-for-c.h array.h)
add_executable(array_test ${ARRAY_SOURCES})

set(DYNAMIC_TEST_SOURCES dynamic-test.c bdd-for-c.h array.h)
add_executable(dynamic_test ${DYNAMIC_TEST_SOURCES})

set(BEFORE_AFTER_SOURCES before-after.c bdd-for-c.h)

add_executable(before_after ${BEFORE_AFTER_SOURCES})

# C++ Example
set(EXAMPLE_CPP_SOURCES example.cpp bdd-for-c.h)
add_executable(example_cpp_test ${EXAMPLE_CPP_SOURCES})
set_target_properties(example_cpp_test PROPERTIES 
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)

# JUnit to HTML Dashboard Tool
set(JUNIT_TO_HTML_SOURCES tools/junit_to_html.c)
add_executable(junit_to_html ${JUNIT_TO_HTML_SOURCES})

target_include_directories(junit_to_html PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/mustache/include
    ${CMAKE_SOURCE_DIR}/parser/internal/json_parser/include
    ${CMAKE_SOURCE_DIR}/parser/internal/cmd_parser/include
    ${CMAKE_SOURCE_DIR}/vendor/cxml/include
)
target_link_libraries(junit_to_html PRIVATE 
    TurboNet::Mustache 
    TurboNet::JsonParser
    TurboNet::CmdParser
    cxml
)

set_target_properties(
  example_test 
  test_tree_test 
  array_test 
  dynamic_test 
  before_after 
  example_cpp_test
  junit_to_html
  PROPERTIES FOLDER "vendor/bdd")
