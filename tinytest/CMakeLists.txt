project(tinytest)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(tinytest INTERFACE)
cmake_config_target(tinytest
  ALIAS TurboNet::TinyTest
  FOLDER "tinytest"
  EXPORT_NAME TinyTest)

target_include_directories(
  tinytest INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                     $<INSTALL_INTERFACE:include>)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/tinytest.h DESTINATION include)

# Examples / self-tests
set(EXAMPLE_SOURCES example.c tinytest.h)
add_executable(example_test ${EXAMPLE_SOURCES})
set(EXAMPLE_SOURCES math_test.c tinytest.h)
add_executable(math_test ${EXAMPLE_SOURCES})
if(UNIX)
  target_link_libraries(math_test PRIVATE m)
endif()

set(TEST_TREE_SOURCES test-tree.c tinytest.h test-tree.h)
add_executable(test_tree_test ${TEST_TREE_SOURCES})

set(ARRAY_SOURCES array.c tinytest.h array.h)
add_executable(array_test ${ARRAY_SOURCES})

set(DYNAMIC_TEST_SOURCES dynamic-test.c tinytest.h array.h)
add_executable(dynamic_test ${DYNAMIC_TEST_SOURCES})

set(BEFORE_AFTER_SOURCES before-after.c tinytest.h)
add_executable(before_after ${BEFORE_AFTER_SOURCES})

set(GROUP_SUITE_SOURCES examples/group_suite_example.c tinytest.h)
add_executable(group_suite_example ${GROUP_SUITE_SOURCES})

set(REGRESSION_RUNTIME_SOURCES regression_runtime.c tinytest.h)
add_executable(tinytest_regression_runtime ${REGRESSION_RUNTIME_SOURCES})
add_test(NAME tinytest_regression_runtime COMMAND tinytest_regression_runtime)

set(FOCUS_OUTPUT_SOURCES focus_output_cases.c tinytest.h)
add_executable(tinytest_focus_output ${FOCUS_OUTPUT_SOURCES})

set(FILTER_OUTPUT_SOURCES filter_output_cases.c tinytest.h)
add_executable(tinytest_filter_output ${FILTER_OUTPUT_SOURCES})

set(JUNIT_COLOR_SOURCES junit_color_cases.c tinytest.h)
add_executable(tinytest_junit_color ${JUNIT_COLOR_SOURCES})

add_test(
  NAME tinytest_focus_tap
  COMMAND ${CMAKE_COMMAND}
          -DTEST_EXE=$<TARGET_FILE:tinytest_focus_output>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_focus_tap.cmake)

add_test(
  NAME tinytest_focus_console
  COMMAND ${CMAKE_COMMAND}
          -DTEST_EXE=$<TARGET_FILE:tinytest_focus_output>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_focus_console.cmake)

add_test(
  NAME tinytest_filter_junit
  COMMAND ${CMAKE_COMMAND}
          -DTEST_EXE=$<TARGET_FILE:tinytest_filter_output>
          -DJUNIT_FILE=${CMAKE_CURRENT_BINARY_DIR}/tinytest-filter.junit.xml
          -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_filter_junit.cmake)

add_test(
  NAME tinytest_junit_color
  COMMAND ${CMAKE_COMMAND}
          -DTEST_EXE=$<TARGET_FILE:tinytest_junit_color>
          -DJUNIT_FILE=${CMAKE_CURRENT_BINARY_DIR}/tinytest-color.junit.xml
          -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_junit_color.cmake)

if(CMAKE_CXX_COMPILER OR CMAKE_CXX_COMPILER_ID)
  set(EXAMPLE_CPP_SOURCES example.cpp tinytest.h)
  add_executable(example_cpp_test ${EXAMPLE_CPP_SOURCES})
  set_target_properties(example_cpp_test PROPERTIES CXX_STANDARD 17
                                                    CXX_STANDARD_REQUIRED ON)

  set(REGRESSION_CPP_SOURCES regression_cpp.cpp tinytest.h)
  add_executable(tinytest_regression_cpp ${REGRESSION_CPP_SOURCES})
  set_target_properties(tinytest_regression_cpp PROPERTIES CXX_STANDARD 17
                                                           CXX_STANDARD_REQUIRED ON)
  add_test(NAME tinytest_regression_cpp COMMAND tinytest_regression_cpp)
endif()

# JUnit to HTML Dashboard Tool
set(JUNIT_TO_HTML_SOURCES tools/junit_to_html.c)
add_executable(junit_to_html ${JUNIT_TO_HTML_SOURCES})

target_include_directories(
  junit_to_html
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_SOURCE_DIR}/mustache/include
          ${CMAKE_SOURCE_DIR}/parser/internal/json_parser/include
          ${CMAKE_SOURCE_DIR}/parser/internal/cmd_parser/include
          ${CMAKE_SOURCE_DIR}/vendor/cxml/include)

target_link_libraries(
  junit_to_html PRIVATE TurboNet::Utils TurboNet::Mustache TurboNet::JsonParser
                        TurboNet::CmdParser cxml)

set(TINYTEST_TARGETS
    example_test
    test_tree_test
    array_test
    dynamic_test
    math_test
    before_after
    group_suite_example
    tinytest_regression_runtime
    tinytest_focus_output
    tinytest_filter_output
    tinytest_junit_color
    junit_to_html)

if(TARGET example_cpp_test)
  list(APPEND TINYTEST_TARGETS example_cpp_test)
endif()

if(TARGET tinytest_regression_cpp)
  list(APPEND TINYTEST_TARGETS tinytest_regression_cpp)
endif()

set_target_properties(${TINYTEST_TARGETS} PROPERTIES FOLDER "tinytest")
