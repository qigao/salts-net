# Detect other test files automatically
cmake_add_source(TEST_SOURCES DIRS ${CMAKE_CURRENT_SOURCE_DIR} PATTERNS "test*.c")

cmake_add_test(
    SOURCES ${TEST_SOURCES}
    FOLDER "parser/${TARGET_NAME}/test"
    LIBS TurboNet::TinyTest ${TARGET_NAME}
)
