# Common module tests

# DNS lookup test (uses c-ares and libuv directly)
add_executable(dns_lookup dns_lookup.c)

target_link_libraries(
  dns_lookup
  PRIVATE TurboNet::Utils
          $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>
          c-ares::cares)

# DNS test using turbo_dns API (requires netcore for turbo_dns.h)
add_executable(test_dns test_dns.c)

target_link_libraries(
  test_dns PRIVATE TurboNet::Utils TurboNet::Core
                   $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>)

# Synchronous DNS example
add_executable(sync_dns_example sync_dns_example.c)

target_link_libraries(
  sync_dns_example
  PRIVATE TurboNet::Utils
          $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>
          c-ares::cares)

# mDNS example
add_executable(mdns_example mdns_example.c)

target_link_libraries(
  mdns_example PRIVATE TurboNet::Utils
                       $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>)

# OpenSSL algorithms test
add_executable(openssl_algorithms openssl_algorithms.c)

target_link_libraries(openssl_algorithms PRIVATE TurboNet::Utils OpenSSL::SSL
                                                 OpenSSL::Crypto)

# Logger example
add_executable(logger_example logger_example.c)

target_link_libraries(logger_example PRIVATE TurboNet::Utils)
# C++ Integration Test
add_executable(cpp_test_tlog cpp_test.cpp)
target_link_libraries(cpp_test_tlog PRIVATE TurboNet::Utils)

add_executable(test_spec test_spec.cpp)
target_link_libraries(test_spec PRIVATE TurboNet::Utils)

# Set folder properties
set_target_properties(
  dns_lookup
  test_dns
  sync_dns_example
  mdns_example
  openssl_algorithms
  logger_example
  cpp_test_tlog
  test_spec
  PROPERTIES FOLDER "shared/utils/examples")
