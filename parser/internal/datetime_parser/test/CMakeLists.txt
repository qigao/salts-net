set(TARGET_NAME test_datetime_parser)

add_executable(${TARGET_NAME} test_datetime_parser.cpp)

target_link_libraries(${TARGET_NAME} PRIVATE TurboNet::DateTimeParser TurboNet::TinyTest)

add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})

set_target_properties(${TARGET_NAME} PROPERTIES FOLDER turbo_parser/parser/datetime_parser/test)
