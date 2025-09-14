/**
 * @file test_crypto_random.c
 * @brief Cryptographically Secure RNG Tests
 */

#include "crypto_random.h"
#include "unity.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void setUp(void) {}
void tearDown(void) {}

void test_crypto_random_basic(void) {
  uint8_t buf[32];
  memset(buf, 0, sizeof(buf));

  int ret = crypto_random_bytes(buf, 32);
  TEST_ASSERT_EQUAL(0, ret);

  // Verify buffer is not all zeros (extremely unlikely with CSPRNG)
  bool all_zero = true;
  for (int i = 0; i < 32; i++) {
    if (buf[i] != 0) {
      all_zero = false;
      break;
    }
  }
  TEST_ASSERT_FALSE(all_zero);
}

void test_crypto_random_different_calls(void) {
  uint8_t buf1[32], buf2[32];

  TEST_ASSERT_EQUAL(0, crypto_random_bytes(buf1, 32));
  TEST_ASSERT_EQUAL(0, crypto_random_bytes(buf2, 32));

  // Two consecutive calls should produce different results
  TEST_ASSERT_NOT_EQUAL(0, memcmp(buf1, buf2, 32));
}

void test_crypto_random_various_sizes(void) {
  uint8_t buf1[1], buf16[16], buf64[64], buf256[256];

  TEST_ASSERT_EQUAL(0, crypto_random_bytes(buf1, 1));
  TEST_ASSERT_EQUAL(0, crypto_random_bytes(buf16, 16));
  TEST_ASSERT_EQUAL(0, crypto_random_bytes(buf64, 64));
  TEST_ASSERT_EQUAL(0, crypto_random_bytes(buf256, 256));
}

void test_crypto_random_null_check(void) {
  TEST_ASSERT_EQUAL(-1, crypto_random_bytes(NULL, 32));

  uint8_t buf[32];
  TEST_ASSERT_EQUAL(-1, crypto_random_bytes(buf, 0));
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_crypto_random_basic);
  RUN_TEST(test_crypto_random_different_calls);
  RUN_TEST(test_crypto_random_various_sizes);
  RUN_TEST(test_crypto_random_null_check);

  return UNITY_END();
}
