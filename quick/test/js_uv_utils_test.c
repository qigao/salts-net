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

static int get_bool_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
    JSValue prop = js_uv_test_global_prop(env, name);
    int result = JS_ToBool(env->ctx, prop);
    JS_FreeValue(env->ctx, prop);
    return result;
}

spec("js_uv_utils") {
    it("should base64 encode string") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var encoded = turbo.base64Encode('Hello, World!');\n");

        const char *result = get_string_global(__bdd_config__, &env, "encoded");
        if (result) {
            check_str_eq("SGVsbG8sIFdvcmxkIQ==", result);
            free((void *)result);
        }
        
        js_uv_test_env_cleanup(&env);
    }

    it("should base64 encode empty string") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var encoded = turbo.base64Encode('');\n");

        const char *result = get_string_global(__bdd_config__, &env, "encoded");
        if (result) {
            check_str_eq("", result);
            free((void *)result);
        }
        
        js_uv_test_env_cleanup(&env);
    }

    it("should base64 decode to string") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var decoded = turbo.base64Decode('SGVsbG8sIFdvcmxkIQ==', 'string');\n");

        const char *result = get_string_global(__bdd_config__, &env, "decoded");
        if (result) {
            check_str_eq("Hello, World!", result);
            free((void *)result);
        }
        
        js_uv_test_env_cleanup(&env);
    }

    it("should base64 decode to arraybuffer") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "const decoded = turbo.base64Decode('SGVsbG8=');\n"
            "var isArrayBuffer = decoded instanceof ArrayBuffer;\n"
            "var length = decoded.byteLength;\n"
            "const view = new Uint8Array(decoded);\n"
            "var firstByte = view[0];\n");

        check_int_ne(0, get_bool_global(__bdd_config__, &env, "isArrayBuffer"));

        JSValue len = js_uv_test_global_prop(&env, "length");
        int32_t length = 0;
        JS_ToInt32(env.ctx, &length, len);
        JS_FreeValue(env.ctx, len);
        check_int_eq(5, length);

        JSValue fb = js_uv_test_global_prop(&env, "firstByte");
        int32_t firstByte = 0;
        JS_ToInt32(env.ctx, &firstByte, fb);
        JS_FreeValue(env.ctx, fb);
        check_int_eq('H', firstByte);
        
        js_uv_test_env_cleanup(&env);
    }

    it("should roundtrip base64 encode/decode") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "const original = 'The quick brown fox jumps over the lazy dog';\n"
            "const encoded = turbo.base64Encode(original);\n"
            "const decoded = turbo.base64Decode(encoded, 'string');\n"
            "var match = (original === decoded);\n");

        check_int_ne(0, get_bool_global(__bdd_config__, &env, "match"));
        
        js_uv_test_env_cleanup(&env);
    }

    it("should base64 encode binary data") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "const data = new Uint8Array([0x00, 0x01, 0x02, 0xFF, 0xFE]);\n"
            "var encoded = turbo.base64Encode(data.buffer);\n");

        const char *result = get_string_global(__bdd_config__, &env, "encoded");
        if (result) {
            check_str_eq("AAEC//4=", result);
            free((void *)result);
        }
        
        js_uv_test_env_cleanup(&env);
    }

    it("should base64 encode special characters") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var encoded = turbo.base64Encode('Hello\\nWorld\\t!');\n");

        const char *result = get_string_global(__bdd_config__, &env, "encoded");
        if (result) {
            check_str_eq("SGVsbG8KV29ybGQJIQ==", result);
            free((void *)result);
        }
        
        js_uv_test_env_cleanup(&env);
    }
}
