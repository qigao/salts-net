/**
 * Unity Test Runner - Main Entry Point
 * "Simple test runner - no fancy frameworks needed"
 */
#include "unity.h"

// External test function declarations
// Error handling tests
extern void test_turbo_set_error_basic(void);
extern void test_turbo_error_callback(void);
extern void test_turbo_clear_error(void);
extern void test_turbo_error_to_string(void);
extern void test_turbo_error_null_params(void);

// File system tests
extern void test_turbo_fs_sync_write_read(void);
extern void test_turbo_fs_write_empty_file(void);
extern void test_turbo_fs_overwrite_file(void);
extern void test_turbo_fs_sync_directory(void);
extern void test_turbo_fs_mkdir_existing_directory(void);
extern void test_turbo_fs_stat_file(void);
extern void test_turbo_fs_stat_nonexistent(void);
extern void test_turbo_fs_unlink_file(void);
extern void test_turbo_fs_unlink_nonexistent(void);
extern void test_turbo_fs_buffer_utilities(void);
extern void test_turbo_fs_path_utilities(void);
extern void test_turbo_fs_get_tmpdir(void);
extern void test_turbo_fs_error_conditions(void);
extern void test_turbo_fs_large_file_handling(void);
extern void test_turbo_fs_binary_data(void);

// Transport operation tests
extern void test_turbo_validate_url(void);
extern void test_turbo_transport_name(void);
extern void test_turbo_handle_init(void);
extern void test_turbo_handle_cleanup(void);
extern void test_turbo_strerror(void);
extern void test_turbo_null_parameters(void);

// Utility function tests
extern void test_string_utilities(void);
extern void test_buffer_utilities(void);
extern void test_url_parsing_edge_cases(void);
extern void test_memory_management(void);
extern void test_thread_safety_basics(void);
extern void test_basic_performance(void);
extern void test_boundary_conditions(void);
extern void test_event_loop_functions(void);
extern void test_event_loop_error_conditions(void);

// Integration scenario tests
extern void test_non_blocking_event_loop_basic(void);
extern void test_user_scenario_pattern(void);
extern void test_multiple_handles_non_blocking(void);
extern void test_error_resilience_non_blocking(void);
extern void test_non_blocking_performance(void);
extern void test_cleanup_and_reinit(void);
extern void test_global_functions_thread_safety(void); 

// Setup functions from test_setup.c
extern void suiteSetUp(void);
extern int suiteTearDown(int num_failures);

int main(int argc, char* argv[])
{
  // Initialize test suite
  suiteSetUp();

  UNITY_BEGIN();

  // Error handling tests
  printf("\n=== Error Handling Tests ===\n");
  RUN_TEST(test_turbo_set_error_basic);
  RUN_TEST(test_turbo_error_callback);
  RUN_TEST(test_turbo_clear_error);
  RUN_TEST(test_turbo_error_to_string);
  RUN_TEST(test_turbo_error_null_params);

  // Transport operation tests
  printf("\n=== Transport Operation Tests ===\n");
  RUN_TEST(test_turbo_validate_url);
  RUN_TEST(test_turbo_transport_name);
  RUN_TEST(test_turbo_handle_init);
  RUN_TEST(test_turbo_handle_cleanup);
  RUN_TEST(test_turbo_strerror);
  RUN_TEST(test_turbo_null_parameters);

  // Utility function tests
  printf("\n=== Utility Function Tests ===\n");
  RUN_TEST(test_string_utilities);
  RUN_TEST(test_buffer_utilities);
  RUN_TEST(test_url_parsing_edge_cases);
  RUN_TEST(test_memory_management);
  RUN_TEST(test_thread_safety_basics);
  RUN_TEST(test_basic_performance);
  RUN_TEST(test_boundary_conditions);
  
  // Event loop function tests
  printf("\n=== Event Loop API Tests ===\n");
  RUN_TEST(test_event_loop_functions);
  RUN_TEST(test_event_loop_error_conditions);
  
  // Integration scenario tests
  printf("\n=== Integration Scenario Tests ===\n");
  RUN_TEST(test_non_blocking_event_loop_basic);
  RUN_TEST(test_user_scenario_pattern);
  RUN_TEST(test_multiple_handles_non_blocking);
  RUN_TEST(test_error_resilience_non_blocking);
  RUN_TEST(test_non_blocking_performance);
  RUN_TEST(test_cleanup_and_reinit);
  RUN_TEST(test_global_functions_thread_safety);

  // File system tests
  printf("\n=== File System Tests ===\n");
  RUN_TEST(test_turbo_fs_sync_write_read);
  RUN_TEST(test_turbo_fs_write_empty_file);
  RUN_TEST(test_turbo_fs_overwrite_file);
  RUN_TEST(test_turbo_fs_sync_directory);
  RUN_TEST(test_turbo_fs_mkdir_existing_directory);
  RUN_TEST(test_turbo_fs_stat_file);
  RUN_TEST(test_turbo_fs_stat_nonexistent);
  RUN_TEST(test_turbo_fs_unlink_file);
  RUN_TEST(test_turbo_fs_unlink_nonexistent);
  RUN_TEST(test_turbo_fs_buffer_utilities);
  RUN_TEST(test_turbo_fs_path_utilities);
  RUN_TEST(test_turbo_fs_get_tmpdir);
  RUN_TEST(test_turbo_fs_error_conditions);
  RUN_TEST(test_turbo_fs_large_file_handling);
  RUN_TEST(test_turbo_fs_binary_data);

  int result = UNITY_END();

  // Cleanup test suite
  return suiteTearDown(result);
}
