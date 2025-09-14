#include "unity.h"

#include "test_uv_fixture.h"
#include <stdint.h>

static JSTurboTestEnv env;

void setUp(void) {
    js_turbo_test_env_init(&env);
}

void tearDown(void) {
    js_turbo_test_env_cleanup(&env);
}

static int32_t get_int_global(const char *name) {
    JSValue prop = js_turbo_test_global_prop(&env, name);
    int32_t value = 0;
    TEST_ASSERT_EQUAL_INT(0, JS_ToInt32(env.ctx, &value, prop));
    JS_FreeValue(env.ctx, prop);
    return value;
}

void test_dns_resolve_localhost(void) {
    js_turbo_test_eval(&env,
                    "globalThis.dnsResult = '';\n"
                    "try {\n"
                    "  globalThis.dnsResult = turbo.dns.resolve('localhost');\n"
                    "} catch (err) {\n"
                    "  globalThis.dnsResult = 'error';\n"
                    "}\n");
    
    JSValue prop = js_turbo_test_global_prop(&env, "dnsResult");
    const char *res = JS_ToCString(env.ctx, prop);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(strcmp(res, "error") == 0);  // Should not be "error"
    TEST_ASSERT_TRUE(strlen(res) > 0);  // Should have some content
    JS_FreeCString(env.ctx, res);
    JS_FreeValue(env.ctx, prop);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_dns_resolve_localhost);
    return UNITY_END();
}
