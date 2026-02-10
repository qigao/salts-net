#include "test_uv_fixture.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *dup_string(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, JSValue value) {
    check_true(JS_IsString(value));
    size_t len = 0;
    const char *str = JS_ToCStringLen(env->ctx, &len, value);
    check_not_null(str);
    char *copy = (char *)malloc(len + 1);
    check_not_null(copy);
    if (copy) {
        memcpy(copy, str, len);
        copy[len] = '\0';
    }
    JS_FreeCString(env->ctx, str);
    return copy;
}

static JSValue get_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
    JSValue prop = js_uv_test_global_prop(env, name);
    check_false(JS_IsException(prop));
    return prop;
}

spec("js_uv_os") {
    it("should verify os queries return values") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
                        "var os = turbo.os;\n"
                        "var osHostname = os.hostname();\n"
                        "var osHomedir = os.homedir();\n"
                        "var osTmpdir = os.tmpdir();\n"
                        "var osUptime = os.uptime();\n"
                        "var osLoad = os.loadavg();\n");


        JSValue hostname = get_global(__bdd_config__, &env, "osHostname");
        char *hostname_str = dup_string(__bdd_config__, &env, hostname);
        if (hostname_str) {
            check_int_ne(0, (int)strlen(hostname_str));
            free(hostname_str);
        }
        JS_FreeValue(env.ctx, hostname);


        JSValue homedir = get_global(__bdd_config__, &env, "osHomedir");
        char *homedir_str = dup_string(__bdd_config__, &env, homedir);
        if (homedir_str) {
            check_int_ne(0, (int)strlen(homedir_str));
            free(homedir_str);
        }
        JS_FreeValue(env.ctx, homedir);

        JSValue tmpdir = get_global(__bdd_config__, &env, "osTmpdir");
        char *tmpdir_str = dup_string(__bdd_config__, &env, tmpdir);
        if (tmpdir_str) {
            check_int_ne(0, (int)strlen(tmpdir_str));
            free(tmpdir_str);
        }
        JS_FreeValue(env.ctx, tmpdir);


        JSValue uptime = get_global(__bdd_config__, &env, "osUptime");
        double uptime_val = 0.0;
        check_int_eq(0, JS_ToFloat64(env.ctx, &uptime_val, uptime));
        check_true(uptime_val >= 0.0);
        JS_FreeValue(env.ctx, uptime);

        JSValue load = get_global(__bdd_config__, &env, "osLoad");
        check_true(JS_IsArray(load));
        int64_t length = 0;
        check_int_eq(0, JS_GetLength(env.ctx, load, &length));
        check_int_eq(3, (int)length);
        JS_FreeValue(env.ctx, load);

        js_uv_test_env_cleanup(&env);
    }
}

