/*
 * cpp_test.cpp
 *
 * Verifies C++ integration with TLog:
 * - Automatic type detection via function overloads (no _Generic)
 * - std::string auto-detection
 * - std::string_view auto-detection (length-bounded, no null required)
 * - Class logging via helpers
 */

#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>


// Include the C header (it has extern "C" wrappers)
#include "tlog.h"
#include "tlog_helper.h"

// Custom class example
struct User {
  int id;
  std::string name;
};

// Helper for custom class
std::string user_to_string(const User &u) {
  return "User(id=" + std::to_string(u.id) + ", name=" + u.name + ")";
}

int main() {
  // 1. Setup logger
  // Note: We use the default logger
  turbo_console_sink_opts_t opts;
  opts.output = stdout;
  opts.use_colors = 1;
  opts.pattern = "[{time_ms}] [{level}] [{thread}] ({file}:{line}) {message}"; // Without component

  turbo_log_sink_t *console = turbo_sink_console_create(&opts);
  tlog_add_sink(tlog_get_default(), console);
  tlog_set_level(tlog_get_default(), TURBO_LOG_LEVEL_DEBUG);

  TLOG_INFO("=== C++ TLog Integration Test ===");

  // 2. Test Basic Types (Auto-detection)
  int i = 42;
  double d = 3.14159;
  bool b = true;
  const char *s = "C-Style String";

  TLOG_INFO("Integer: {}", i);
  TLOG_INFO("Double: {:.2f}", d); // Modifiers work too!
  TLOG_INFO("Bool: {}", b);
  TLOG_INFO("C-String: {}", s);

  // 3. Test std::string (The SFINAE template magic)
  std::string cpp_str = "std::string content";
  TLOG_INFO("std::string: {}", cpp_str);

  // 3b. Test std::string_view
  std::string_view sv = "std::string_view content";
  TLOG_INFO("string_view: {}", sv);

  // 3c. Test std::string_view from substring (no null terminator)
  std::string base = "hello world";
  std::string_view partial = std::string_view(base).substr(0, 5);
  TLOG_INFO("string_view substr: {}", partial);

  // 4. Test Mixed
  TLOG_INFO("Mixed: {} | {} | {}", i, cpp_str, b);

  // 4b. Test Enum (Implicit Cast)
  enum Color { RED = 1, GREEN = 2, BLUE = 3 };
  Color c = GREEN;
  enum class Status : uint16_t { OK = 200, TERROR = 404 };
  Status s_code = Status::OK;

  TLOG_INFO("Enum (Old-style): {}", c);
  TLOG_INFO("Enum Class (Typed): {}", ENUM_NAME(s_code));

  // 5. Test Custom Class (via helper)
  User u = {100, "Alice"};
  // We can call the helper inline
  TLOG_INFO("Custom Class: {}", user_to_string(u));

  // 6. Test Modifiers on std::string
  // Note: Modifier is applied to the underlying const char*
  std::string long_str = "truncated";
  TLOG_INFO("Precision on std::string: {:.4s}", long_str);

  // 7. Test Containers (Recursive)
  // Map
  std::map<std::string, int> scores = {{"Alice", 100}, {"Bob", 85}};
  TLOG_INFO("Scores Map: {}", tlog::format(scores));

  // Nested Vector
  std::vector<std::vector<int>> matrix = {{1, 2}, {3, 4}};
  TLOG_INFO("Nested Matrix: {}", tlog::format(matrix));

  // Mixed Complex
  std::map<std::string, std::vector<int>> user_data = {{"User1", {10, 20}},
                                                       {"User2", {30, 40, 50}}};
  TLOG_INFO("User Data: {}", tlog::format(user_data));

  // 8. Test Time Types
  // std::chrono::system_clock::now() auto-detection
  auto now = std::chrono::system_clock::now();
  TLOG_INFO("Chrono now: {}", now);

  // Custom format with chrono
  TLOG_INFO("Chrono HH:MM:SS: {:%H:%M:%S}", now);

  // turbo_timeval_t auto-detection
  turbo_timeval_t tv;
  tv.tv_sec = 1700000000;
  tv.tv_usec = 123000;
  TLOG_INFO("turbo_timeval_t: {}", tv);

  TLOG_INFO("=== Test Complete ===");

  tlog_destroy(tlog_get_default());
  return 0;
}
