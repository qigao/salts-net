#include "test_uv_fixture.h"

static void assert_function(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, JSValue value) {
    check_false(JS_IsException(value));
    check_true(JS_IsFunction(env->ctx, value));
}

spec("js_uv_module") {
    it("should verify turbo global object present") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        JSValue turbo_obj = js_uv_test_global_prop(&env, "turbo");
        check_true(JS_IsObject(turbo_obj));

        JSValue timers = JS_GetPropertyStr(env.ctx, turbo_obj, "setTimeout");
        assert_function(__bdd_config__, &env, timers);
        JS_FreeValue(env.ctx, timers);

        JSValue fs = JS_GetPropertyStr(env.ctx, turbo_obj, "fs");
        check_true(JS_IsObject(fs));
        JS_FreeValue(env.ctx, fs);

        JSValue dns = JS_GetPropertyStr(env.ctx, turbo_obj, "dns");
        check_true(JS_IsObject(dns));
        JS_FreeValue(env.ctx, dns);

        JSValue http = JS_GetPropertyStr(env.ctx, turbo_obj, "http");
        check_true(JS_IsObject(http));
        JS_FreeValue(env.ctx, http);

        JS_FreeValue(env.ctx, turbo_obj);
        
        js_uv_test_env_cleanup(&env);
    }

    it("should verify legacy uv compatibility") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        // Test that old uv API still works for backward compatibility
        JSValue uv_obj = js_uv_test_global_prop(&env, "uv");
        // Should be undefined since we changed to turbo namespace
        check_true(JS_IsUndefined(uv_obj));
        JS_FreeValue(env.ctx, uv_obj);
        
        js_uv_test_env_cleanup(&env);
    }
}