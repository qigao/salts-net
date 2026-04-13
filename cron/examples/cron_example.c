#include "turbo_cron.h"
#include "turbo_thread.h"

#include <stdio.h>

static void print_local_time(const char *prefix, time_t value) {
  char buf[32];
  if (turbo_cron_format_time(value, buf, sizeof(buf), NULL) < 0) {
    printf("%s<format-error>\n", prefix);
    return;
  }
  printf("%s%s\n", prefix, buf);
}

static void print_fire(const turbo_cron_expr_t *expr, time_t scheduled_at, void *user_data) {
  (void)expr;
  (void)user_data;
  print_local_time("cron fired at ", scheduled_at);
}

int main(void) {
  turbo_cron_expr_t expr_obj;
  turbo_cron_runner_t *runner;
  const char *expr = "*/2 * * * *";
  time_t next_times[5];
  int count;
  int i;

  if (turbo_cron_parse(expr, &expr_obj) != TURBO_CRON_OK) {
    fprintf(stderr, "failed to parse cron expression '%s'\n", expr);
    return 1;
  }

  count = turbo_cron_next_n(&expr_obj, time(NULL), next_times, 5);
  if (count < 0) {
    fprintf(stderr, "failed to compute future fire times for '%s'\n", expr);
    return 1;
  }

  printf("next %d fire times for '%s':\n", count, expr);
  for (i = 0; i < count; ++i) {
    print_local_time("  - ", next_times[i]);
  }

  runner = turbo_cron_runner_create(expr, print_fire, NULL);
  if (!runner) {
    fprintf(stderr, "failed to create cron runner for '%s'\n", expr);
    return 1;
  }

  if (turbo_cron_runner_start(runner) != TURBO_CRON_OK) {
    fprintf(stderr, "failed to start cron runner\n");
    turbo_cron_runner_destroy(runner);
    return 1;
  }

  printf("running cron '%s' for about 5 minutes\n", expr);
  turbo_sleep_ms(5 * 60 * 1000);

  turbo_cron_runner_stop(runner);
  turbo_cron_runner_destroy(runner);
  return 0;
}
