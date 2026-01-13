# Common module tests

# DNS lookup test (uses c-ares and libuv directly)
add_executable(dns_lookup dns_lookup.c)

target_link_libraries(dns_lookup PRIVATE TurboNet::Common libuv::uv
                                         c-ares::cares)

# DNS test using turbo_dns API (requires netcore for turbo_dns.h)
add_executable(test_dns test_dns.c)

target_link_libraries(test_dns PRIVATE TurboNet::Common TurboNet::Core
                                       libuv::uv)

# Synchronous DNS example
add_executable(sync_dns_example sync_dns_example.c)

target_link_libraries(sync_dns_example PRIVATE TurboNet::Common libuv::uv
                                               c-ares::cares)

# mDNS example
add_executable(mdns_example mdns_example.c)

target_link_libraries(mdns_example PRIVATE TurboNet::Common libuv::uv)

# OpenSSL algorithms test
add_executable(openssl_algorithms openssl_algorithms.c)

target_link_libraries(openssl_algorithms PRIVATE TurboNet::Common OpenSSL::SSL
                                                 OpenSSL::Crypto)

# Logger example
add_executable(logger_example logger_example.c)

target_link_libraries(logger_example PRIVATE TurboNet::Common )

add_executable(bitmap_stats bitmap_stats_example.c)
target_link_libraries(bitmap_stats PRIVATE TurboNet::Common)
# Set folder properties
set_target_properties(
  dns_lookup
  test_dns
  sync_dns_example
  mdns_example
  openssl_algorithms
  logger_example
  bitmap_stats
  PROPERTIES FOLDER "common/examples")
