#include "test_uv_fixture.h"

#include <stdlib.h>
#include <string.h>

static char *dup_string_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
    JSValue prop = js_uv_test_global_prop(env, name);
    check_true(JS_IsString(prop));
    size_t len = 0;
    const char *str = JS_ToCStringLen(env->ctx, &len, prop);
    check_not_null(str);
    char *copy = (char *)malloc(len + 1);
    check_not_null(copy);
    if (copy) {
        memcpy(copy, str, len);
        copy[len] = '\0';
    }
    JS_FreeCString(env->ctx, str);
    JS_FreeValue(env->ctx, prop);
    return copy;
}

static int get_bool_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
    JSValue prop = js_uv_test_global_prop(env, name);
    int result = JS_ToBool(env->ctx, prop);
    JS_FreeValue(env->ctx, prop);
    return result;
}

spec("js_uv_signal") {
    it("should watch and stop signal and close handle") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
                        "var signalStatus = 'pending';\n"
                        "var signalClosed = false;\n"
                        "var watcher = turbo.signal.watch(2, () => { signalStatus = 'fired'; });\n"
                        "watcher.onClose(() => { signalClosed = true; });\n"
                        "watcher.stop();\n");
        
        // Run loop until signalClosed becomes true (max 10 iterations)
        for (int i = 0; i < 10 && !get_bool_global(__bdd_config__, &env, "signalClosed"); i++) {
             js_uv_test_run_loop(&env);
        }

        char *status = dup_string_global(__bdd_config__, &env, "signalStatus");
        check_str_eq("pending", status);
        free(status);
        check_int_ne(0, get_bool_global(__bdd_config__, &env, "signalClosed"));

        // Clean up the watcher reference
        js_uv_test_eval(&env, "watcher = null;\n");

        js_uv_test_env_cleanup(&env);
    }
}