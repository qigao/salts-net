# Common module tests C++ Integration Test
add_executable(cpp_test_tlog cpp_test.cpp)
target_link_libraries(cpp_test_tlog PRIVATE TurboNet::TinyTest TurboNet::Utils)
set_target_properties(cpp_test_tlog PROPERTIES FOLDER "tinytest")

add_executable(test_spec test_spec.cpp)
target_link_libraries(test_spec PRIVATE TurboNet::TinyTest TurboNet::Utils)
set_target_properties(test_spec PROPERTIES FOLDER "tinytest")

# FS example
add_executable(fs_example fs_example.c)
target_link_libraries(fs_example PRIVATE TurboNet::TinyTest TurboNet::Utils)
set_target_properties(fs_example PROPERTIES FOLDER "tinytest")
