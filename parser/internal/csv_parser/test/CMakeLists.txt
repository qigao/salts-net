set(TEST_TARGET test_csv_parser)

add_executable(${TEST_TARGET}
    test_csv_parser.c
)

target_link_libraries(${TEST_TARGET} PRIVATE
    csv_parser
    TurboNet::TinyTest
)

add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})

set_target_properties(${TEST_TARGET} PROPERTIES
    FOLDER turbo_parser/parser/csv_parser
)
