#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base64_utils.h"
#include "unity.h"

void setUp(void) {}

void tearDown(void) {}

void test_base64_encode_known_value(void) {
  const uint8_t input[] = "TurboNet";
  char *encoded = NULL;

  int rc = tn_base64_encode(input, sizeof(input) - 1, &encoded);
  TEST_ASSERT_EQUAL(0, rc);
  TEST_ASSERT_NOT_NULL(encoded);
  TEST_ASSERT_EQUAL_STRING("VHVyYm9OZXQ=", encoded);

  free(encoded);
}

void test_base64_decode_known_value(void) {
  const char *encoded = "Zm9vYmFy";
  uint8_t *decoded = NULL;
  size_t decoded_len = 0;

  int rc = tn_base64_decode(encoded, &decoded, &decoded_len);
  TEST_ASSERT_EQUAL(0, rc);
  TEST_ASSERT_NOT_NULL(decoded);
  TEST_ASSERT_EQUAL_size_t(6, decoded_len);
  TEST_ASSERT_EQUAL_MEMORY("foobar", decoded, decoded_len);

  free(decoded);
}

void test_base64_decode_invalid_input(void) {
  const char *encoded = "invalid*data";
  uint8_t *decoded = NULL;
  size_t decoded_len = 0;

  int rc = tn_base64_decode(encoded, &decoded, &decoded_len);
  TEST_ASSERT_EQUAL(-1, rc);
  TEST_ASSERT_NULL(decoded);
}

void test_base64_null_parameters(void) {
  char *encoded = NULL;
  uint8_t *decoded = NULL;
  size_t decoded_len = 0;

  TEST_ASSERT_EQUAL(-1, tn_base64_encode(NULL, 4, &encoded));
  TEST_ASSERT_EQUAL(-1, tn_base64_encode((const uint8_t *)"data", 4, NULL));
  TEST_ASSERT_EQUAL(-1, tn_base64_decode(NULL, &decoded, &decoded_len));
  TEST_ASSERT_EQUAL(-1, tn_base64_decode("Zg==", NULL, &decoded_len));
  TEST_ASSERT_EQUAL(-1, tn_base64_decode("Zg==", &decoded, NULL));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_base64_encode_known_value);
  RUN_TEST(test_base64_decode_known_value);
  RUN_TEST(test_base64_decode_invalid_input);
  RUN_TEST(test_base64_null_parameters);
  return UNITY_END();
}
