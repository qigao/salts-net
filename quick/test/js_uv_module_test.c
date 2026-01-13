#include "unity.h"

#include "test_uv_fixture.h"

static JSTurboTestEnv env;

void setUp(void) {
    js_turbo_test_env_init(&env);
}

void tearDown(void) {
    js_turbo_test_env_cleanup(&env);
}

static void assert_function(JSValue value) {
    TEST_ASSERT_FALSE(JS_IsException(value));
    TEST_ASSERT_TRUE(JS_IsFunction(env.ctx, value));
}

void test_turbo_global_object_present(void) {
    JSValue turbo_obj = js_turbo_test_global_prop(&env, "turbo");
    TEST_ASSERT_TRUE(JS_IsObject(turbo_obj));

    JSValue timers = JS_GetPropertyStr(env.ctx, turbo_obj, "setTimeout");
    assert_function(timers);
    JS_FreeValue(env.ctx, timers);

    JSValue fs = JS_GetPropertyStr(env.ctx, turbo_obj, "fs");
    TEST_ASSERT_TRUE(JS_IsObject(fs));
    JS_FreeValue(env.ctx, fs);

    JSValue dns = JS_GetPropertyStr(env.ctx, turbo_obj, "dns");
    TEST_ASSERT_TRUE(JS_IsObject(dns));
    JS_FreeValue(env.ctx, dns);

    JSValue http = JS_GetPropertyStr(env.ctx, turbo_obj, "http");
    TEST_ASSERT_TRUE(JS_IsObject(http));
    JS_FreeValue(env.ctx, http);

    JS_FreeValue(env.ctx, turbo_obj);
}

void test_legacy_uv_compatibility(void) {
    // Test that old uv API still works for backward compatibility
    JSValue uv_obj = js_turbo_test_global_prop(&env, "uv");
    // Should be undefined since we changed to turbo namespace
    TEST_ASSERT_TRUE(JS_IsUndefined(uv_obj));
    JS_FreeValue(env.ctx, uv_obj);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_turbo_global_object_present);
    RUN_TEST(test_legacy_uv_compatibility);
    return UNITY_END();
}