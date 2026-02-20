cmake_minimum_required(VERSION 3.10)

project(turbo_script)

find_package(tinytest)

include_directories(
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../exprtk_parser/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../strtk_parser/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../datetime_parser/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../json_parser/include
    ${CMAKE_SOURCE_DIR}/http/include
    ${CMAKE_SOURCE_DIR}/netcore/include
    ${CMAKE_SOURCE_DIR}/shared/utils/include
 )

set(SOURCES
    src/turbo_script.c
)

add_library(turbo_script STATIC ${SOURCES})
target_link_libraries(turbo_script PRIVATE
    exprtk_parser
    strtk_parser
    datetime_parser
    json_parser
    http_coro_client
    PUBLIC TurboNet::Utils TurboNet::Core
)

add_executable(test_turbo_script test/test_turbo_script.c)
target_link_libraries(test_turbo_script PRIVATE
    turbo_script
    http_coro_client
    exprtk_parser # Transitive?
    strtk_parser
    datetime_parser
    json_parser
    TurboNet::TinyTest
)

add_test(NAME test_turbo_script COMMAND test_turbo_script)

set_target_properties(turbo_script PROPERTIES
    FOLDER turbo_parser/parser/turbo_script
)
set_target_properties(test_turbo_script PROPERTIES
    FOLDER turbo_parser/parser/turbo_script
)
