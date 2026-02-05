cmake_minimum_required(VERSION 3.5)
project(bdd)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c99 -W -Wall")

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

set_target_properties(
  example_test 
  test_tree_test 
  array_test 
  dynamic_test 
  before_after 
  PROPERTIES FOLDER "vendor/bdd")
