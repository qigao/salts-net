set(TEST_TARGET test_csv_parser)

add_executable(${TEST_TARGET} test_csv_parser.c)

target_link_libraries(${TEST_TARGET} PRIVATE csv_parser TurboNet::TinyTest)

add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})

set_target_properties(${TEST_TARGET} PROPERTIES FOLDER
                                                parser/csv_parser)

add_executable(test_csv_stream_processor test_csv_stream_processor.c)
target_link_libraries(test_csv_stream_processor
                      PRIVATE TurboNet::TinyTest csv_parser ${TARGET_NAME_LIB})
add_test(NAME test_csv_stream_processor COMMAND test_csv_stream_processor)
set_target_properties(test_csv_stream_processor
                      PROPERTIES FOLDER parser/csv_parser)

add_executable(test_dsv_filter test_dsv_filter.c)
target_link_libraries(test_dsv_filter PRIVATE TurboNet::TinyTest csv_parser
                                              ${TARGET_NAME_LIB})
add_test(NAME test_dsv_filter COMMAND test_dsv_filter)
set_target_properties(test_dsv_filter PROPERTIES FOLDER
                                                 parser/csv_parser)
