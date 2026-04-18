#include "platform.h"
#include "tinytest.h"
#include "turbo_thread.h"
#include <stdatomic.h>
static int count = 0;
static atomic_int destroy_race_count = 0;

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

static void on_destroy_race_timer(turbo_timer_t *timer) {
  atomic_int *counter = (atomic_int *)turbo_timer_get_data(timer);

  if (counter != NULL) {
    atomic_fetch_add(counter, 1);
  }
  turbo_sleep_ms(1);
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

  it("should tolerate destroy racing with one-shot expiry") {
    atomic_store(&destroy_race_count, 0);

    for (int i = 0; i < 256; ++i) {
      turbo_timer_t *timer = turbo_timer_create(NULL);

      check_not_null(timer);
      turbo_timer_set_data(timer, &destroy_race_count);
      check_int_eq(turbo_timer_start(timer, on_destroy_race_timer, 1, 0), 0);
      turbo_sleep_ms(1);
      turbo_timer_destroy(timer);
    }

    check_int_ge(atomic_load(&destroy_race_count), 0);
  }
}
