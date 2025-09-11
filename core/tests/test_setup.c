/**
 * Test Setup - Unity framework integration
 * "Testing is not about proving code works, it's about proving it doesn't
 * break"
 */
#include "log.h"
#include "turbonet.h"
#include "unity.h"

void setUp(void)
{
  // Each test starts with a clean slate
}

void tearDown(void)
{
  // Clean up after each test
}

void suiteSetUp(void)
{
  // No need to manually initialize - TurboNet is now fully autonomous!

  log_set_level(LOG_ERROR);  // Reduce noise during tests
}

int suiteTearDown(int num_failures)
{
  return num_failures;
}
