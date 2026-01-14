/**
 * @file js_uv_multi_runtime_test.c
 * @brief Test multiple runtime/context creation and destruction cycles
 *
 * This test verifies that:
 * 1. Multiple runtimes can be created and destroyed
 * 2. Class IDs are properly registered per-runtime
 * 3. No use-after-free or memory corruption occurs
 */
#include "unity.h"
#include "test_uv_fixture.h"

#include <stdlib.h>
#include <string.h>

void setUp(void) {
}

void tearDown(void) {
}

void test_single_runtime_init_cleanup(void) {
    JSRuntime *rt = JS_NewRuntime();
    TEST_ASSERT_NOT_NULL(rt);

    JSContext *ctx = JS_NewContext(rt);
    TEST_ASSERT_NOT_NULL(ctx);

    int result = js_init_turbo_module(ctx);
    TEST_ASSERT_EQUAL_INT(0, result);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

void test_multiple_sequential_runtimes(void) {
    // Create and destroy multiple runtimes sequentially
    for (int i = 0; i < 3; i++) {
        JSRuntime *rt = JS_NewRuntime();
        TEST_ASSERT_NOT_NULL_MESSAGE(rt, "Failed to create runtime");

        JSContext *ctx = JS_NewContext(rt);
        TEST_ASSERT_NOT_NULL_MESSAGE(ctx, "Failed to create context");

        int result = js_init_turbo_module(ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, result, "Failed to init turbo module");

        // Verify turbo object exists
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue turbo = JS_GetPropertyStr(ctx, global, "turbo");
        TEST_ASSERT_FALSE(JS_IsUndefined(turbo));
        TEST_ASSERT_FALSE(JS_IsException(turbo));

        JS_FreeValue(ctx, turbo);
        JS_FreeValue(ctx, global);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }
}

void test_multiple_contexts_same_runtime(void) {
    JSRuntime *rt = JS_NewRuntime();
    TEST_ASSERT_NOT_NULL(rt);

    // Create multiple contexts on the same runtime
    for (int i = 0; i < 3; i++) {
        JSContext *ctx = JS_NewContext(rt);
        TEST_ASSERT_NOT_NULL_MESSAGE(ctx, "Failed to create context");

        int result = js_init_turbo_module(ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, result, "Failed to init turbo module");

        // Verify turbo object exists
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue turbo = JS_GetPropertyStr(ctx, global, "turbo");
        TEST_ASSERT_FALSE(JS_IsUndefined(turbo));

        JS_FreeValue(ctx, turbo);
        JS_FreeValue(ctx, global);

        JS_FreeContext(ctx);
    }

    JS_FreeRuntime(rt);
}

void test_concurrent_runtimes(void) {
    // Create multiple runtimes that exist at the same time
    JSRuntime *rt1 = JS_NewRuntime();
    JSRuntime *rt2 = JS_NewRuntime();
    TEST_ASSERT_NOT_NULL(rt1);
    TEST_ASSERT_NOT_NULL(rt2);

    JSContext *ctx1 = JS_NewContext(rt1);
    JSContext *ctx2 = JS_NewContext(rt2);
    TEST_ASSERT_NOT_NULL(ctx1);
    TEST_ASSERT_NOT_NULL(ctx2);

    // Initialize both
    TEST_ASSERT_EQUAL_INT(0, js_init_turbo_module(ctx1));
    TEST_ASSERT_EQUAL_INT(0, js_init_turbo_module(ctx2));

    // Verify both work
    JSValue global1 = JS_GetGlobalObject(ctx1);
    JSValue turbo1 = JS_GetPropertyStr(ctx1, global1, "turbo");
    TEST_ASSERT_FALSE(JS_IsUndefined(turbo1));

    JSValue global2 = JS_GetGlobalObject(ctx2);
    JSValue turbo2 = JS_GetPropertyStr(ctx2, global2, "turbo");
    TEST_ASSERT_FALSE(JS_IsUndefined(turbo2));

    JS_FreeValue(ctx1, turbo1);
    JS_FreeValue(ctx1, global1);
    JS_FreeValue(ctx2, turbo2);
    JS_FreeValue(ctx2, global2);

    // Clean up in different order than creation
    JS_FreeContext(ctx2);
    JS_FreeRuntime(rt2);
    JS_FreeContext(ctx1);
    JS_FreeRuntime(rt1);
}

void test_eval_after_init(void) {
    JSRuntime *rt = JS_NewRuntime();
    TEST_ASSERT_NOT_NULL(rt);

    JSContext *ctx = JS_NewContext(rt);
    TEST_ASSERT_NOT_NULL(ctx);

    TEST_ASSERT_EQUAL_INT(0, js_init_turbo_module(ctx));

    // Test basic eval
    const char *code = "var result = typeof turbo; result;";
    JSValue val = JS_Eval(ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_GLOBAL);
    TEST_ASSERT_FALSE(JS_IsException(val));

    const char *str = JS_ToCString(ctx, val);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_EQUAL_STRING("object", str);

    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, val);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

void test_turbo_submodules_exist(void) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    TEST_ASSERT_EQUAL_INT(0, js_init_turbo_module(ctx));

    const char *code =
        "var checks = {"
        "  hasFs: typeof turbo.fs === 'object',"
        "  hasDns: typeof turbo.dns === 'object',"
        "  hasHttp: typeof turbo.http === 'object',"
        "  hasOs: typeof turbo.os === 'object',"
        "  hasSignal: typeof turbo.signal === 'object',"
        "  hasProc: typeof turbo.proc === 'object',"
        "  hasNet: typeof turbo.net === 'object'"
        "};"
        "checks;";

    JSValue val = JS_Eval(ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_GLOBAL);
    TEST_ASSERT_FALSE(JS_IsException(val));

    JSValue hasFs = JS_GetPropertyStr(ctx, val, "hasFs");
    JSValue hasDns = JS_GetPropertyStr(ctx, val, "hasDns");
    JSValue hasHttp = JS_GetPropertyStr(ctx, val, "hasHttp");
    JSValue hasOs = JS_GetPropertyStr(ctx, val, "hasOs");
    JSValue hasSignal = JS_GetPropertyStr(ctx, val, "hasSignal");
    JSValue hasProc = JS_GetPropertyStr(ctx, val, "hasProc");
    JSValue hasNet = JS_GetPropertyStr(ctx, val, "hasNet");

    TEST_ASSERT_TRUE(JS_ToBool(ctx, hasFs));
    TEST_ASSERT_TRUE(JS_ToBool(ctx, hasDns));
    TEST_ASSERT_TRUE(JS_ToBool(ctx, hasHttp));
    TEST_ASSERT_TRUE(JS_ToBool(ctx, hasOs));
    TEST_ASSERT_TRUE(JS_ToBool(ctx, hasSignal));
    TEST_ASSERT_TRUE(JS_ToBool(ctx, hasProc));
    TEST_ASSERT_TRUE(JS_ToBool(ctx, hasNet));

    JS_FreeValue(ctx, hasFs);
    JS_FreeValue(ctx, hasDns);
    JS_FreeValue(ctx, hasHttp);
    JS_FreeValue(ctx, hasOs);
    JS_FreeValue(ctx, hasSignal);
    JS_FreeValue(ctx, hasProc);
    JS_FreeValue(ctx, hasNet);
    JS_FreeValue(ctx, val);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_single_runtime_init_cleanup);
    RUN_TEST(test_multiple_sequential_runtimes);
    RUN_TEST(test_multiple_contexts_same_runtime);
    RUN_TEST(test_concurrent_runtimes);
    RUN_TEST(test_eval_after_init);
    RUN_TEST(test_turbo_submodules_exist);
    return UNITY_END();
}
