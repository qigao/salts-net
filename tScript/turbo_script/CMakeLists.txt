cmake_minimum_required(VERSION 3.10)

project(turbo_script) 

include_directories(
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../fin/include    # fin.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../exprtk/include # exprtk_types.h etc.
    ${CMAKE_SOURCE_DIR}/http/include
    ${CMAKE_SOURCE_DIR}/shared/utils/include
 )

set(SOURCES
    src/turbo_script.c
    src/dsv_filter.c
    src/csv_stream_processor.c
)

add_library(turbo_script SHARED ${SOURCES})
add_library(TurboNet::TurboScript ALIAS turbo_script)

target_compile_definitions(turbo_script PRIVATE SHARED_CXX)

target_link_libraries(turbo_script PRIVATE
    exprtk
    fin
    datetime_parser
    json_parser
    csv_parser
    http_coro_client
    PUBLIC TurboNet::Utils TurboNet::Core
)

add_executable(test_turbo_script test/test_turbo_script.c)
target_link_libraries(test_turbo_script PRIVATE
    turbo_script
    TurboNet::TinyTest
)

add_test(NAME test_turbo_script COMMAND test_turbo_script)

add_executable(test_csv_stream_processor test/test_csv_stream_processor.c)
target_link_libraries(test_csv_stream_processor PRIVATE
    turbo_script csv_parser
    TurboNet::TinyTest
)

add_test(NAME test_csv_stream_processor COMMAND test_csv_stream_processor)

set_target_properties(turbo_script PROPERTIES
    FOLDER turbo_script
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME TurboScript
)
set_target_properties(test_turbo_script PROPERTIES
    FOLDER turbo_script
)
set_target_properties(test_csv_stream_processor PROPERTIES
    FOLDER turbo_script
)

# Install library
install(
  TARGETS turbo_script
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

# Install headers
install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION include
  FILES_MATCHING
  PATTERN "*.h")
