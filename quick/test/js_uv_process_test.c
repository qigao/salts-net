#include "test_uv_fixture.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static int32_t get_int_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
    JSValue prop = js_uv_test_global_prop(env, name);
    int32_t value = -999;
    int result = JS_ToInt32(env->ctx, &value, prop);
    JS_FreeValue(env->ctx, prop);
    if (result != 0) {
        check_int_eq(result, 0);  // Will fail and return
        return -999;  // Fallback
    }
    return value;
}

static const char *dup_string_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
    JSValue prop = js_uv_test_global_prop(env, name);
    if (!JS_IsString(prop)) {
        check_true(JS_IsString(prop));  // Will fail and return
        JS_FreeValue(env->ctx, prop);
        return NULL;  // Fallback
    }
    size_t len = 0;
    const char *str = JS_ToCStringLen(env->ctx, &len, prop);
    if (!str) {
        check_not_null(str);  // Will fail and return
        JS_FreeValue(env->ctx, prop);
        return NULL;  // Fallback
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        check_not_null(copy);  // Will fail and return
        JS_FreeCString(env->ctx, str);
        JS_FreeValue(env->ctx, prop);
        return NULL;  // Fallback
    }
    memcpy(copy, str, len);
    copy[len] = '\0';
    JS_FreeCString(env->ctx, str);
    JS_FreeValue(env->ctx, prop);
    return copy;
}

spec("js_uv_process") {
    it("should return exit code from process spawn") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
                        "var procExit = -999;\n"
                        "var procStdout = null;\n"
                        "turbo.proc.spawn({ file: 'cmd.exe', args: ['cmd.exe', '/C', 'echo.'] })\n"
                        "  .then((res) => {\n"
                        "    procExit = res.exitCode;\n"
                        "    procStdout = res.stdout;\n"
                        "  })\n"
                        "  .catch((e) => { procExit = -1; });\n");

        // Run the event loop until the process completes (max 200 iterations)
        for (int i = 0; i < 200 && get_int_global(__bdd_config__, &env, "procExit") == -999; i++) {
            js_uv_test_run_loop(&env);
#ifdef _WIN32
            Sleep(10);
#else
            usleep(10000);
#endif
        }

        check_int_eq(get_int_global(__bdd_config__, &env, "procExit"), 0);
        const char *stdout_str = dup_string_global(__bdd_config__, &env, "procStdout");
        /* cmd.exe writes CRLF; ensure string is not empty */
        if (stdout_str) {
            check_int_ne(0, (int)strlen(stdout_str));
            free((void *)stdout_str);
        }

        js_uv_test_env_cleanup(&env);
    }
}
