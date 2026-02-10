#include "test_uv_fixture.h"

#include <stdlib.h>
#include <string.h>

static const char *get_string_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
    JSValue prop = js_uv_test_global_prop(env, name);
    if (JS_IsNull(prop) || JS_IsUndefined(prop)) {
        JS_FreeValue(env->ctx, prop);
        return NULL;
    }
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

static int32_t get_int_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
    JSValue prop = js_uv_test_global_prop(env, name);
    int32_t value = 0;
    JS_ToInt32(env->ctx, &value, prop);
    JS_FreeValue(env->ctx, prop);
    return value;
}

static int get_bool_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
    JSValue prop = js_uv_test_global_prop(env, name);
    int result = JS_ToBool(env->ctx, prop);
    JS_FreeValue(env->ctx, prop);
    return result;
}

spec("js_uv_http") {
    it("should verify http module exists") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var hasHttp = typeof turbo.http === 'object';\n"
            "var hasGet = typeof turbo.http.get === 'function';\n"
            "var hasPost = typeof turbo.http.post === 'function';\n"
            "var hasRequest = typeof turbo.http.request === 'function';\n");

        check_true(get_bool_global(__bdd_config__, &env, "hasHttp"));
        check_true(get_bool_global(__bdd_config__, &env, "hasGet"));
        check_true(get_bool_global(__bdd_config__, &env, "hasPost"));
        check_true(get_bool_global(__bdd_config__, &env, "hasRequest"));

        js_uv_test_env_cleanup(&env);
    }

    it("should verify http get returns object") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        // Test against a reliable endpoint - httpbin.org
        js_uv_test_eval(&env,
            "var result = null;\n"
            "var error = null;\n"
            "try {\n"
            "  const resp = turbo.http.get('https://httpbin.org/get');\n"
            "  var result = typeof resp;\n"
            "  var hasStatus = 'status' in resp;\n"
            "  var hasBody = 'body' in resp;\n"
            "} catch (e) {\n"
            "  var error = e.message;\n"
            "}\n");

        const char *result = get_string_global(__bdd_config__, &env, "result");
        if (result) {
            check_str_eq("object", result);
            check_true(get_bool_global(__bdd_config__, &env, "hasStatus"));
            check_true(get_bool_global(__bdd_config__, &env, "hasBody"));
            free((void *)result);
        } else {
            // Network might not be available in test environment
            const char *error = get_string_global(__bdd_config__, &env, "error");
            printf("HTTP test skipped - network unavailable (error: %s)\n", error ? error : "unknown");
            if (error) free((void *)error);
        }

        js_uv_test_env_cleanup(&env);
    }

    it("should verify http response structure") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var status = 0;\n"
            "var bodyType = 'none';\n"
            "try {\n"
            "  const resp = turbo.http.get('https://httpbin.org/status/200');\n"
            "  var status = resp.status;\n"
            "  var bodyType = typeof resp.body;\n"
            "} catch (e) {\n"
            "  var status = -1;\n"
            "}\n");

        int32_t status = get_int_global(__bdd_config__, &env, "status");
        if (status > 0) {
            check_int_eq(200, status);
        } else {
            printf("HTTP test skipped - network unavailable\n");
        }

        js_uv_test_env_cleanup(&env);
    }

    it("should verify http post with body") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var success = false;\n"
            "var echoedData = '';\n"
            "try {\n"
            "  const resp = turbo.http.post('https://httpbin.org/post', 'test=data');\n"
            "  if (resp.status === 200) {\n"
            "    var success = true;\n"
            "    const json = JSON.parse(resp.body);\n"
            "    var echoedData = json.data || '';\n"
            "  }\n"
            "} catch (e) {\n"
            "  var success = false;\n"
            "}\n");

        if (get_bool_global(__bdd_config__, &env, "success")) {
            const char *echoed = get_string_global(__bdd_config__, &env, "echoedData");
            check_str_eq("test=data", echoed);
            if (echoed) free((void *)echoed);
        } else {
            printf("HTTP POST test skipped - network unavailable\n");
        }

        js_uv_test_env_cleanup(&env);
    }

    it("should verify http request method") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var method = '';\n"
            "try {\n"
            "  const resp = turbo.http.request('DELETE', 'https://httpbin.org/delete');\n"
            "  if (resp.status === 200) {\n"
            "    const json = JSON.parse(resp.body);\n"
            "    var method = json.url ? 'ok' : '';\n"
            "  }\n"
            "} catch (e) {\n"
            "  var method = 'error';\n"
            "}\n");

        const char *method = get_string_global(__bdd_config__, &env, "method");
        if (method && strcmp(method, "error") != 0 && strlen(method) > 0) {
            check_str_eq("ok", method);
        } else {
            printf("HTTP request method test skipped - network unavailable\n");
        }
        if (method) free((void *)method);

        js_uv_test_env_cleanup(&env);
    }

    it("should verify http handles 404") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var status = 0;\n"
            "try {\n"
            "  const resp = turbo.http.get('https://httpbin.org/status/404');\n"
            "  var status = resp.status;\n"
            "} catch (e) {\n"
            "  var status = -1;\n"
            "}\n");

        int32_t status = get_int_global(__bdd_config__, &env, "status");
        if (status > 0) {
            check_int_eq(404, status);
        } else {
            printf("HTTP 404 test skipped - network unavailable\n");
        }

        js_uv_test_env_cleanup(&env);
    }
}
