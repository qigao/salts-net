project(tinytest)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(tinytest INTERFACE)
add_library(TurboNet::TinyTest ALIAS tinytest)
target_include_directories(tinytest INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)
# Set target properties
set_target_properties(
  tinytest
  PROPERTIES FOLDER "vendor/tinytest"
             EXPORT_NAME TinyTest)
# Install
install(
    TARGETS tinytest
    EXPORT TurboNetTargets)

install(
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/tinytest.h
    DESTINATION include)

# Examples / self-tests
set(EXAMPLE_SOURCES example.c tinytest.h)
add_executable(example_test ${EXAMPLE_SOURCES})

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

if(CMAKE_CXX_COMPILER OR CMAKE_CXX_COMPILER_ID)
    set(EXAMPLE_CPP_SOURCES example.cpp tinytest.h)
    add_executable(example_cpp_test ${EXAMPLE_CPP_SOURCES})
    set_target_properties(example_cpp_test PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
endif()

# JUnit to HTML Dashboard Tool
set(JUNIT_TO_HTML_SOURCES tools/junit_to_html.c)
add_executable(junit_to_html ${JUNIT_TO_HTML_SOURCES})

target_include_directories(junit_to_html PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/utils/include
    ${CMAKE_SOURCE_DIR}/mustache/include
    ${CMAKE_SOURCE_DIR}/parser/internal/json_parser/include
    ${CMAKE_SOURCE_DIR}/parser/internal/cmd_parser/include
    ${CMAKE_SOURCE_DIR}/vendor/cxml/include)

target_link_libraries(junit_to_html PRIVATE
    TurboNet::Mustache
    TurboNet::JsonParser
    TurboNet::CmdParser
    cxml)

set(TINYTEST_TARGETS
    example_test
    test_tree_test
    array_test
    dynamic_test
    before_after
    group_suite_example
    junit_to_html
)

if(TARGET example_cpp_test)
    list(APPEND TINYTEST_TARGETS example_cpp_test)
endif()

set_target_properties(${TINYTEST_TARGETS}
    PROPERTIES FOLDER "vendor/tinytest")
