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

static int get_bool_global(const char *name) {
    JSValue prop = js_turbo_test_global_prop(&env, name);
    int result = JS_ToBool(env.ctx, prop);
    JS_FreeValue(env.ctx, prop);
    return result;
}

void test_base64_encode_string(void) {
    js_turbo_test_eval(&env,
        "var encoded = turbo.base64Encode('Hello, World!');\n");

    const char *result = get_string_global("encoded");
    TEST_ASSERT_EQUAL_STRING("SGVsbG8sIFdvcmxkIQ==", result);
    free((void *)result);
}

void test_base64_encode_empty_string(void) {
    js_turbo_test_eval(&env,
        "var encoded = turbo.base64Encode('');\n");

    const char *result = get_string_global("encoded");
    TEST_ASSERT_EQUAL_STRING("", result);
    free((void *)result);
}

void test_base64_decode_to_string(void) {
    js_turbo_test_eval(&env,
        "var decoded = turbo.base64Decode('SGVsbG8sIFdvcmxkIQ==', 'string');\n");

    const char *result = get_string_global("decoded");
    TEST_ASSERT_EQUAL_STRING("Hello, World!", result);
    free((void *)result);
}

void test_base64_decode_to_arraybuffer(void) {
    js_turbo_test_eval(&env,
        "const decoded = turbo.base64Decode('SGVsbG8=');\n"
        "var isArrayBuffer = decoded instanceof ArrayBuffer;\n"
        "var length = decoded.byteLength;\n"
        "const view = new Uint8Array(decoded);\n"
        "var firstByte = view[0];\n");

    TEST_ASSERT_TRUE(get_bool_global("isArrayBuffer"));

    JSValue len = js_turbo_test_global_prop(&env, "length");
    int32_t length = 0;
    JS_ToInt32(env.ctx, &length, len);
    JS_FreeValue(env.ctx, len);
    TEST_ASSERT_EQUAL_INT(5, length);

    JSValue fb = js_turbo_test_global_prop(&env, "firstByte");
    int32_t firstByte = 0;
    JS_ToInt32(env.ctx, &firstByte, fb);
    JS_FreeValue(env.ctx, fb);
    TEST_ASSERT_EQUAL_INT('H', firstByte);
}

void test_base64_roundtrip(void) {
    js_turbo_test_eval(&env,
        "const original = 'The quick brown fox jumps over the lazy dog';\n"
        "const encoded = turbo.base64Encode(original);\n"
        "const decoded = turbo.base64Decode(encoded, 'string');\n"
        "var match = (original === decoded);\n");

    TEST_ASSERT_TRUE(get_bool_global("match"));
}

void test_base64_encode_binary_data(void) {
    js_turbo_test_eval(&env,
        "const data = new Uint8Array([0x00, 0x01, 0x02, 0xFF, 0xFE]);\n"
        "var encoded = turbo.base64Encode(data.buffer);\n");

    const char *result = get_string_global("encoded");
    TEST_ASSERT_EQUAL_STRING("AAEC//4=", result);
    free((void *)result);
}

void test_base64_special_characters(void) {
    js_turbo_test_eval(&env,
        "var encoded = turbo.base64Encode('Hello\\nWorld\\t!');\n");

    const char *result = get_string_global("encoded");
    TEST_ASSERT_EQUAL_STRING("SGVsbG8KV29ybGQJIQ==", result);
    free((void *)result);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_base64_encode_string);
    RUN_TEST(test_base64_encode_empty_string);
    RUN_TEST(test_base64_decode_to_string);
    RUN_TEST(test_base64_decode_to_arraybuffer);
    RUN_TEST(test_base64_roundtrip);
    RUN_TEST(test_base64_encode_binary_data);
    RUN_TEST(test_base64_special_characters);
    return UNITY_END();
}
