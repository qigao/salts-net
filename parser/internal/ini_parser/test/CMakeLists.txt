# Detect other test files automatically
turbonet_collect_files(TEST_SOURCES DIRS ${CMAKE_CURRENT_SOURCE_DIR} PATTERNS "test*.c")

turbonet_add_tests(
    SOURCES ${TEST_SOURCES}
    FOLDER "turbo_parser/parser/${TARGET_NAME}/test"
    LIBS TurboNet::TinyTest ${TARGET_NAME}
)
