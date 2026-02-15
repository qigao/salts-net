#include "test_uv_fixture.h"

#include <stdlib.h>
#include <string.h>

static const char *get_string_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
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

spec("js_uv_string") {
    it("should expose string utils under turbo.string") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var slug = turbo.string.slugify('Hello World');\n"
            "var cap = turbo.string.capitalize('hello world');\n"
            "var trunc = turbo.string.truncate('hello world', 5);\n");

        const char *slug = get_string_global(__bdd_config__, &env, "slug");
        check_str_eq("hello-world", slug);
        free((void *)slug);

        const char *cap = get_string_global(__bdd_config__, &env, "cap");
        check_str_eq("Hello World", cap);
        free((void *)cap);

        const char *trunc = get_string_global(__bdd_config__, &env, "trunc");
        check_str_eq("he...", trunc);
        free((void *)trunc);
        
        js_uv_test_env_cleanup(&env);
    }

    it("should maintain backward compatibility with global strUtils") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var slug = strUtils.slugify('Backward Compat');\n");

        const char *slug = get_string_global(__bdd_config__, &env, "slug");
        check_str_eq("backward-compat", slug);
        free((void *)slug);
        
        js_uv_test_env_cleanup(&env);
    }

    it("should support template substitution") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var result = turbo.string.template('Hello {{name}}!', { name: 'Turbo' });\n");

        const char *result = get_string_global(__bdd_config__, &env, "result");
        check_str_eq("Hello Turbo!", result);
        free((void *)result);
        
        js_uv_test_env_cleanup(&env);
    }
}
