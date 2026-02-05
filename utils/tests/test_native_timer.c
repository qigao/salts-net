/**
 * @file test_native_timer.c
 * @brief Example demonstrating native OS timer usage (uses OS thread pool)
 */

#include "platform.h"
#include "tlog.h"
#include <stdio.h>

// Example timer callback
static void on_timer_tick(turbo_timer_t *timer) {
  int *count = (int *)turbo_timer_get_data(timer);
  if (count) {
    (*count)++;
    printf("Native timer fired! Count: %d\n", *count);
    
    // Stop after 5 ticks
    if (*count >= 5) {
      printf("Stopping native timer after 5 ticks\n");
      turbo_timer_stop(timer);
    }
  }
}

// One-shot timer callback
static void on_oneshot_timer(turbo_timer_t *timer) {
  const char *msg = (const char *)turbo_timer_get_data(timer);
  printf("Native one-shot timer fired: %s\n", msg ? msg : "no message");
}

int main(void) {
  printf("=== Native OS Timer Test ===\n");
  printf("Using system timer facilities (aliased to standard turbo_timer_* API):\n");
#ifdef _WIN32
  printf("  Platform: Windows (CreateTimerQueueTimer)\n");
#else
  printf("  Platform: POSIX (timer_create with SIGEV_THREAD)\n");
#endif
  printf("\n");
  
  // Test 1: Repeating timer
  printf("Test 1: Creating repeating timer (500ms interval)\n");
  // Note: NULL loop passed - native timers ignore it
  turbo_timer_t *repeating = turbo_timer_create(NULL);
  if (!repeating) {
    fprintf(stderr, "Failed to create repeating timer\n");
    return 1;
  }
  
  int count = 0;
  turbo_timer_set_data(repeating, &count);
  turbo_timer_start(repeating, on_timer_tick, 500, 500);
  
  // Test 2: One-shot timer
  printf("Test 2: Creating one-shot timer (1000ms)\n");
  turbo_timer_t *oneshot = turbo_timer_create(NULL);
  if (!oneshot) {
    fprintf(stderr, "Failed to create one-shot timer\n");
    turbo_timer_destroy(repeating);
    return 1;
  }
  
  const char *message = "Hello from native OS timer!";
  turbo_timer_set_data(oneshot, (void *)message);
  turbo_timer_start(oneshot, on_oneshot_timer, 1000, 0);
  
  // Wait for timers to complete
  printf("\nWaiting for timers to fire...\n\n");
  turbo_sleep_ms(3500);  // Wait 3.5 seconds
  
  // Cleanup
  printf("\nCleaning up timers...\n");
  turbo_timer_destroy(repeating);
  turbo_timer_destroy(oneshot);
  
  printf("\n=== Test Complete ===\n");
  printf("Note: Native OS timers use system thread pool - no custom threads!\n");
  return 0;
}
