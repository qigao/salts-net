#include "turbo_cron.h"

#include <stdio.h>

int main(void) {
  turbo_cron_expr_t expr;
  time_t next_fire = 0;
  char buf[32];

  if (turbo_cron_parse("*/30 * * * *", &expr) != TURBO_CRON_OK) {
    return 1;
  }

  if (turbo_cron_next(&expr, time(NULL), &next_fire) != TURBO_CRON_OK) {
    return 2;
  }

  if (turbo_cron_format_time(next_fire, buf, sizeof(buf), NULL) < 0) {
    return 3;
  }

  printf("next fire: %s\n", buf);
  return 0;
}
