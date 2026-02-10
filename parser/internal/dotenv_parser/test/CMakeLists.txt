set(TEST_NAME test_dotenv)

add_executable(${TEST_NAME}
    test_dotenv.c
)

target_link_libraries(${TEST_NAME}
    PRIVATE
        TurboNet::DotEnvParser
        TurboNet::TinyTest
)

target_include_directories(${TEST_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

set_target_properties(${TEST_NAME} PROPERTIES
    FOLDER   turbo_parser/parser/dotenv_parser/test
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
