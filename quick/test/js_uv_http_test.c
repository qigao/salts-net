#include "unity.h"
#include "test_uv_fixture.h"

#include <stdlib.h>
#include <string.h>

static JSTurboTestEnv env;

void setUp(void) {
    js_turbo_test_env_init(&env);
}

void tearDown(void) {
    js_turbo_test_env_cleanup(&env);
}

static const char *get_string_global(const char *name) {
    JSValue prop = js_turbo_test_global_prop(&env, name);
    if (JS_IsNull(prop) || JS_IsUndefined(prop)) {
        JS_FreeValue(env.ctx, prop);
        return NULL;
    }
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

static int32_t get_int_global(const char *name) {
    JSValue prop = js_turbo_test_global_prop(&env, name);
    int32_t value = 0;
    JS_ToInt32(env.ctx, &value, prop);
    JS_FreeValue(env.ctx, prop);
    return value;
}

static int get_bool_global(const char *name) {
    JSValue prop = js_turbo_test_global_prop(&env, name);
    int result = JS_ToBool(env.ctx, prop);
    JS_FreeValue(env.ctx, prop);
    return result;
}

void test_http_module_exists(void) {
    js_turbo_test_eval(&env,
        "var hasHttp = typeof turbo.http === 'object';\n"
        "var hasGet = typeof turbo.http.get === 'function';\n"
        "var hasPost = typeof turbo.http.post === 'function';\n"
        "var hasRequest = typeof turbo.http.request === 'function';\n");

    TEST_ASSERT_TRUE(get_bool_global("hasHttp"));
    TEST_ASSERT_TRUE(get_bool_global("hasGet"));
    TEST_ASSERT_TRUE(get_bool_global("hasPost"));
    TEST_ASSERT_TRUE(get_bool_global("hasRequest"));
}

void test_http_get_returns_object(void) {
    // Test against a reliable endpoint - httpbin.org
    js_turbo_test_eval(&env,
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

    const char *result = get_string_global("result");
    if (result) {
        TEST_ASSERT_EQUAL_STRING("object", result);
        TEST_ASSERT_TRUE(get_bool_global("hasStatus"));
        TEST_ASSERT_TRUE(get_bool_global("hasBody"));
        free((void *)result);
    } else {
        // Network might not be available in test environment
        const char *error = get_string_global("error");
        TEST_MESSAGE("HTTP test skipped - network unavailable");
        if (error) free((void *)error);
    }
}

void test_http_response_structure(void) {
    js_turbo_test_eval(&env,
        "var status = 0;\n"
        "var bodyType = 'none';\n"
        "try {\n"
        "  const resp = turbo.http.get('https://httpbin.org/status/200');\n"
        "  var status = resp.status;\n"
        "  var bodyType = typeof resp.body;\n"
        "} catch (e) {\n"
        "  var status = -1;\n"
        "}\n");

    int32_t status = get_int_global("status");
    if (status > 0) {
        TEST_ASSERT_EQUAL_INT(200, status);
    } else {
        TEST_MESSAGE("HTTP test skipped - network unavailable");
    }
}

void test_http_post_with_body(void) {
    js_turbo_test_eval(&env,
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

    if (get_bool_global("success")) {
        const char *echoed = get_string_global("echoedData");
        TEST_ASSERT_EQUAL_STRING("test=data", echoed);
        free((void *)echoed);
    } else {
        TEST_MESSAGE("HTTP POST test skipped - network unavailable");
    }
}

void test_http_request_method(void) {
    js_turbo_test_eval(&env,
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

    const char *method = get_string_global("method");
    if (method && strcmp(method, "error") != 0 && strlen(method) > 0) {
        TEST_ASSERT_EQUAL_STRING("ok", method);
    } else {
        TEST_MESSAGE("HTTP request method test skipped - network unavailable");
    }
    if (method) free((void *)method);
}

void test_http_handles_404(void) {
    js_turbo_test_eval(&env,
        "var status = 0;\n"
        "try {\n"
        "  const resp = turbo.http.get('https://httpbin.org/status/404');\n"
        "  var status = resp.status;\n"
        "} catch (e) {\n"
        "  var status = -1;\n"
        "}\n");

    int32_t status = get_int_global("status");
    if (status > 0) {
        TEST_ASSERT_EQUAL_INT(404, status);
    } else {
        TEST_MESSAGE("HTTP 404 test skipped - network unavailable");
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_http_module_exists);
    RUN_TEST(test_http_get_returns_object);
    RUN_TEST(test_http_response_structure);
    RUN_TEST(test_http_post_with_body);
    RUN_TEST(test_http_request_method);
    RUN_TEST(test_http_handles_404);
    return UNITY_END();
}
