#include "platform.h"
#include "tinytest.h"
#include <stdio.h>
#include "turbo_thread.h"
static int count = 0;

static void on_timer_tick(turbo_timer_t *timer) {
  int *c = (int *)turbo_timer_get_data(timer);
  if (c) {
    (*c)++;
    if (*c >= 5) {
      turbo_timer_stop(timer);
    }
  }
}

static void on_oneshot_timer(turbo_timer_t *timer) {
  int *c = (int *)turbo_timer_get_data(timer);
  if (c) {
    (*c)++;
  }
}

spec("Native Timer Tests") {

  it("should handle repeating native timers") {
    count = 0;
    turbo_timer_t *repeating = turbo_timer_create(NULL);
    check_not_null(repeating);

    turbo_timer_set_data(repeating, &count);
    turbo_timer_start(repeating, on_timer_tick, 100, 100);

    turbo_sleep_ms(1000); // Wait for ticks

    check_int_ge(count, 5);
    turbo_timer_destroy(repeating);
  }

  it("should handle one-shot native timers") {
    count = 0;
    turbo_timer_t *oneshot = turbo_timer_create(NULL);
    check_not_null(oneshot);

    turbo_timer_set_data(oneshot, &count);
    turbo_timer_start(oneshot, on_oneshot_timer, 100, 0);

    turbo_sleep_ms(300); // Wait for firing

    check_int_eq(count, 1);
    turbo_timer_destroy(oneshot);
  }
}
