#include "unity.h"

// Memory management test declarations (from test_memory.c)
void test_url_struct_is_stack_allocated(void);
void test_parse_url_with_null_inputs(void);
void test_multiple_parses_same_struct(void);
void test_copy_substring_function(void);

// Parser test declarations (from test_parser.c)
void test_parse_simple_http_url(void);
void test_parse_https_url_with_path(void);
void test_parse_url_with_port(void);
void test_parse_url_with_query(void);
void test_parse_url_with_fragment(void);
void test_parse_url_with_userinfo(void);
void test_parse_ipv4_address(void);
void test_parse_ipv6_address(void);
void test_parse_complex_url(void);
void test_parse_percent_encoded_characters(void);
void test_parse_url_without_port(void);
void test_parse_url_with_empty_query(void);

// Edge cases test declarations (from test_edge_cases.c)
void test_parse_empty_string(void);
void test_parse_malformed_scheme(void);
void test_parse_missing_scheme(void);
void test_parse_invalid_port(void);
void test_parse_non_numeric_port(void);
void test_parse_very_long_scheme(void);
void test_parse_very_long_hostname(void);
void test_parse_very_long_path(void);
void test_parse_malformed_ipv6(void);
void test_parse_empty_host(void);
void test_parse_deeply_nested_path(void);
void test_parse_url_with_null_bytes(void);
void test_parse_url_with_special_characters(void);
void test_parse_scheme_only(void);
void test_parse_valid_edge_cases(void);
void test_error_handling_robustness(void);

// network parser
void test_parse_tcp_ipv4_url(void);
void test_parse_tls_ipv4_url(void);
void test_parse_udp_ipv4_url(void);
void test_parse_tcp_ipv6_url(void);
void test_parse_tls_ipv6_url(void);
void test_parse_udp_ipv6_with_zone(void);

void test_parse_tcp_domain_url(void);
void test_parse_tls_domain_url(void);
void test_parse_https_alias_url(void);
void test_parse_tcp_no_port(void);
void test_parse_tls_no_port(void);
void test_parse_pipe_unix_url(void);
void test_parse_pipe_relative_url(void);
void test_parse_pipe_absolute_path_url(void);
void test_parse_pipe_windows_url(void);
void test_parse_kcp_url(void);
void test_parse_quic_url(void);
void test_parse_http3_alias_url(void);
void test_parse_empty_url(void);
void test_parse_no_scheme_url(void);
void test_parse_empty_scheme_url(void);
void test_parse_malformed_ipv6_url(void);
void test_parse_empty_host_url(void);

int main(void)
{
  UNITY_BEGIN();

  // Memory management tests (now stack-based!)
  RUN_TEST(test_url_struct_is_stack_allocated);
  RUN_TEST(test_parse_url_with_null_inputs);
  RUN_TEST(test_multiple_parses_same_struct);
  RUN_TEST(test_copy_substring_function);

  // Basic parsing tests
  RUN_TEST(test_parse_simple_http_url);
  RUN_TEST(test_parse_https_url_with_path);
  RUN_TEST(test_parse_url_with_port);
  RUN_TEST(test_parse_url_with_query);
  RUN_TEST(test_parse_url_with_fragment);
  RUN_TEST(test_parse_url_with_userinfo);

  // IP address tests
  RUN_TEST(test_parse_ipv4_address);
  RUN_TEST(test_parse_ipv6_address);

  // Complex URL tests
  RUN_TEST(test_parse_complex_url);
  RUN_TEST(test_parse_percent_encoded_characters);
  RUN_TEST(test_parse_url_without_port);
  RUN_TEST(test_parse_url_with_empty_query);

  // Edge cases and security tests
  RUN_TEST(test_parse_empty_string);
  RUN_TEST(test_parse_malformed_scheme);
  RUN_TEST(test_parse_missing_scheme);
  RUN_TEST(test_parse_invalid_port);
  RUN_TEST(test_parse_non_numeric_port);
  RUN_TEST(test_parse_very_long_scheme);
  RUN_TEST(test_parse_very_long_hostname);
  RUN_TEST(test_parse_very_long_path);
  RUN_TEST(test_parse_malformed_ipv6);
  RUN_TEST(test_parse_empty_host);

  // Stress tests
  RUN_TEST(test_parse_deeply_nested_path);
  RUN_TEST(test_parse_url_with_null_bytes);
  RUN_TEST(test_parse_url_with_special_characters);
  RUN_TEST(test_parse_scheme_only);
  RUN_TEST(test_parse_valid_edge_cases);
  RUN_TEST(test_error_handling_robustness);
  // IPv4 address tests
  RUN_TEST(test_parse_tcp_ipv4_url);
  RUN_TEST(test_parse_tls_ipv4_url);
  RUN_TEST(test_parse_udp_ipv4_url);

  // IPv6 address tests
  RUN_TEST(test_parse_tcp_ipv6_url);
  RUN_TEST(test_parse_tls_ipv6_url);
  RUN_TEST(test_parse_udp_ipv6_with_zone);

  // Domain name tests
  RUN_TEST(test_parse_tcp_domain_url);
  RUN_TEST(test_parse_tls_domain_url);
  RUN_TEST(test_parse_https_alias_url);

  // Default port tests
  RUN_TEST(test_parse_tcp_no_port);
  RUN_TEST(test_parse_tls_no_port);

  // PIPE transport tests
  RUN_TEST(test_parse_pipe_unix_url);
  RUN_TEST(test_parse_pipe_relative_url);
  RUN_TEST(test_parse_pipe_absolute_path_url);
  RUN_TEST(test_parse_pipe_windows_url);

  // Other transport tests
  RUN_TEST(test_parse_kcp_url);
  RUN_TEST(test_parse_quic_url);
  RUN_TEST(test_parse_http3_alias_url);

  // Invalid URL tests
  RUN_TEST(test_parse_empty_url);
  RUN_TEST(test_parse_no_scheme_url);
  RUN_TEST(test_parse_empty_scheme_url);
  RUN_TEST(test_parse_malformed_ipv6_url);
  RUN_TEST(test_parse_empty_host_url);

  return UNITY_END();
}
