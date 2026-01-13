#include "unity.h"

#include "test_uv_fixture.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static JSUVTestEnv env;

void setUp(void) {
    js_uv_test_env_init(&env);
}

void tearDown(void) {
    js_uv_test_env_cleanup(&env);
}

static int32_t get_int_global(const char *name) {
    JSValue prop = js_uv_test_global_prop(&env, name);
    int32_t value = -999;
    TEST_ASSERT_EQUAL_INT(0, JS_ToInt32(env.ctx, &value, prop));
    JS_FreeValue(env.ctx, prop);
    return value;
}

static const char *dup_string_global(const char *name) {
    JSValue prop = js_uv_test_global_prop(&env, name);
    TEST_ASSERT_TRUE(JS_IsString(prop));
    size_t len = 0;
    const char *str = JS_ToCStringLen(env.ctx, &len, prop);
    TEST_ASSERT_NOT_NULL(str);
    char *copy = (char *)malloc(len + 1);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, str, len);
    copy[len] = '\0';
    JS_FreeCString(env.ctx, str);
    JS_FreeValue(env.ctx, prop);
    return copy;
}

void test_process_spawn_returns_exit_code(void) {
    js_uv_test_eval(&env,
                    "var procExit = -999;\n"
                    "var procStdout = null;\n"
                    "turbo.proc.spawn({ file: 'cmd.exe', args: ['cmd.exe', '/C', 'echo.'] })\n"
                    "  .then((res) => {\n"
                    "    procExit = res.exitCode;\n"
                    "    procStdout = res.stdout;\n"
                    "  })\n"
                    "  .catch((e) => { procExit = -1; });\n");

    // Run the event loop until the process completes (max 50 iterations)
    for (int i = 0; i < 50 && get_int_global("procExit") == -999; i++) {
        js_uv_test_run_loop(&env);
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
    }

    TEST_ASSERT_EQUAL_INT(0, get_int_global("procExit"));
    const char *stdout_str = dup_string_global("procStdout");
    /* cmd.exe writes CRLF; ensure string is not empty */
    TEST_ASSERT_NOT_EQUAL(0, (int)strlen(stdout_str));
    free((void *)stdout_str);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_process_spawn_returns_exit_code);
    return UNITY_END();
}
