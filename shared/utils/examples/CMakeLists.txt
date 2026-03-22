# Common module tests

# OpenSSL algorithms test
add_executable(openssl_algorithms openssl_algorithms.c)

target_link_libraries(openssl_algorithms PRIVATE TurboNet::Utils OpenSSL::SSL
                                                 OpenSSL::Crypto)

# C++ Integration Test
add_executable(cpp_test_tlog cpp_test.cpp)
target_link_libraries(cpp_test_tlog PRIVATE TurboNet::Utils)

add_executable(test_spec test_spec.cpp)
target_link_libraries(test_spec PRIVATE TurboNet::Utils)

# FS example
add_executable(fs_example fs_example.c)
target_link_libraries(fs_example PRIVATE TurboNet::Utils)

# Set folder properties
set_target_properties(openssl_algorithms cpp_test_tlog test_spec fs_example
                      PROPERTIES FOLDER "shared/utils/examples")
