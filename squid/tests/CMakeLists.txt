# Squid Tests
add_executable(test_squid test_squid.c)

target_link_libraries(test_squid PRIVATE squid TurboNet::TinyTest)

target_include_directories(test_squid PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# Register test
add_test(NAME squid_tests COMMAND test_squid)

# Set test properties
set_target_properties(test_squid PROPERTIES
    FOLDER "squid/tests"
    C_STANDARD 11
)

# Windows specific
if(MSVC)
    target_compile_options(test_squid PRIVATE /std:c11 /experimental:c11atomics)
    target_compile_definitions(test_squid PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
