#include <stdio.h>
#include <tlog.h>


int main() {
  // 1. Initialize logger configuration
  tlog_config_t config = {
      .min_level = TURBO_LOG_LEVEL_DEBUG,
      .async_mode = 1,          // Enable async logging
      .buffer_size = 64 * 1024, // 64KB async buffer
      .pool_size = 16 * 1024    // 16KB formatting pool
  };

  // 2. Create the logger
  tlog_t *logger = tlog_create(&config);
  if (!logger) {
    fprintf(stderr, "Failed to create logger\n");
    return 1;
  }

  // 3. Add a colored console sink with a custom pattern
  turbo_console_sink_opts_t console_opts = {
      .output = stdout, .use_colors = 1, .pattern = "[{time_ms}] [{level}] [{thread}] {message}"};
  tlog_add_sink(logger, turbo_sink_console_create(&console_opts));

  // 4. Set as default logger for TLOG_* macros
  tlog_set_default(logger);

  // 5. Start logging!
  TLOG_INFO("Application started");
  TLOG_DEBUG("User ID: {}, Action: {}", 1234, "login");

  for (int i = 0; i < 5; i++) {
    TLOG_INFO("Processing batch {}...", i);
  }

  TLOG_WARN("Workload increasing, current queue size: {}", tlog_get_queue_size(logger));

  // 6. Clean up
  // This will flush all sinks and wait for async queue to drain
  tlog_destroy(logger);

  return 0;
}
