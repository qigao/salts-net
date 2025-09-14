#include "test_uv_fixture.h"
#include <stdint.h>
#include "unity.h"
#include <stdlib.h>
#include <string.h>

static JSTurboTestEnv env;
void setUp(void) { js_turbo_test_env_init(&env); }
void tearDown(void) { js_turbo_test_env_cleanup(&env); }

void test_buffer_append_grows_buffer(void) {
  JSTurboByteBuffer buf;
  js_turbo_buffer_init(&buf);
  const uint8_t sample[] = {1, 2, 3, 4};
  TEST_ASSERT_EQUAL_INT(0, js_turbo_buffer_append(&buf, sample, sizeof(sample)));
  TEST_ASSERT_EQUAL_UINT(sizeof(sample), buf.length);
  TEST_ASSERT_NOT_NULL(buf.data);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(sample, buf.data, sizeof(sample));
  js_turbo_buffer_free(&buf);
  TEST_ASSERT_NULL(buf.data);
  TEST_ASSERT_EQUAL_UINT(0, buf.length);
  TEST_ASSERT_EQUAL_UINT(0, buf.capacity);
}

void test_collect_data_from_string(void) {
  JSValue str_val = JS_NewString(env.ctx, "hello");
  TEST_ASSERT_FALSE(JS_IsException(str_val));
  uint8_t *data = NULL;
  size_t len = 0;
  TEST_ASSERT_EQUAL_INT(0, js_turbo_collect_data(env.ctx, str_val, &data, &len));
  TEST_ASSERT_NOT_NULL(data);
  TEST_ASSERT_EQUAL_UINT(5, len);
  TEST_ASSERT_EQUAL_UINT8('h', data[0]);
  TEST_ASSERT_EQUAL_UINT8('o', data[4]);
  free(data);
  JS_FreeValue(env.ctx, str_val);
}

void test_make_turbo_error_sets_properties(void) {
  JSValue error = js_turbo_make_error(env.ctx, -1, "turbo_test_syscall");
  TEST_ASSERT_FALSE(JS_IsException(error));
  TEST_ASSERT_TRUE(JS_IsObject(error));
  JSValue msg_val = JS_GetPropertyStr(env.ctx, error, "message");
  TEST_ASSERT_FALSE(JS_IsException(msg_val));
  const char *msg = JS_ToCString(env.ctx, msg_val);
  TEST_ASSERT_NOT_NULL(msg);
  TEST_ASSERT_GREATER_THAN_UINT(0, (uint32_t)strlen(msg));
  JS_FreeCString(env.ctx, msg);
  JS_FreeValue(env.ctx, msg_val);
  JS_FreeValue(env.ctx, error);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_buffer_append_grows_buffer);
  RUN_TEST(test_collect_data_from_string);
  RUN_TEST(test_make_turbo_error_sets_properties);
  return UNITY_END();
}
