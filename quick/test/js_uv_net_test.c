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

static int get_bool_global(const char *name) {
    JSValue prop = js_turbo_test_global_prop(&env, name);
    int result = JS_ToBool(env.ctx, prop);
    JS_FreeValue(env.ctx, prop);
    return result;
}

static const char *get_string_global(const char *name) {
    JSValue prop = js_turbo_test_global_prop(&env, name);
    if (JS_IsNull(prop) || JS_IsUndefined(prop)) {
        JS_FreeValue(env.ctx, prop);
        return NULL;
    }
    if (!JS_IsString(prop)) {
        JS_FreeValue(env.ctx, prop);
        return NULL;
    }
    size_t len = 0;
    const char *str = JS_ToCStringLen(env.ctx, &len, prop);
    char *copy = (char *)malloc(len + 1);
    if (copy) {
        memcpy(copy, str, len);
        copy[len] = '\0';
    }
    JS_FreeCString(env.ctx, str);
    JS_FreeValue(env.ctx, prop);
    return copy;
}

void test_net_module_exists(void) {
    js_turbo_test_eval(&env,
        "var hasNet = typeof turbo.net === 'object';\n"
        "var hasTcpClient = typeof turbo.net.TcpClient === 'function';\n"
        "var hasWebSocket = typeof turbo.net.WebSocket === 'function';\n");

    TEST_ASSERT_TRUE(get_bool_global("hasNet"));
    TEST_ASSERT_TRUE(get_bool_global("hasTcpClient"));
    TEST_ASSERT_TRUE(get_bool_global("hasWebSocket"));
}

void test_tcp_client_constructor(void) {
    js_turbo_test_eval(&env,
        "var clientCreated = false;\n"
        "var clientType = '';\n"
        "try {\n"
        "  const client = new turbo.net.TcpClient();\n"
        "  var clientCreated = true;\n"
        "  var clientType = typeof client;\n"
        "  var hasConnect = typeof client.connect === 'function';\n"
        "  var hasSend = typeof client.send === 'function';\n"
        "  var hasClose = typeof client.close === 'function';\n"
        "  var hasOn = typeof client.on === 'function';\n"
        "} catch (e) {\n"
        "  var clientCreated = false;\n"
        "  var error = e.message;\n"
        "}\n");

    TEST_ASSERT_TRUE(get_bool_global("clientCreated"));

    const char *type = get_string_global("clientType");
    TEST_ASSERT_EQUAL_STRING("object", type);
    free((void *)type);

    TEST_ASSERT_TRUE(get_bool_global("hasConnect"));
    TEST_ASSERT_TRUE(get_bool_global("hasSend"));
    TEST_ASSERT_TRUE(get_bool_global("hasClose"));
    TEST_ASSERT_TRUE(get_bool_global("hasOn"));
}

void test_tcp_client_on_returns_this(void) {
    js_turbo_test_eval(&env,
        "var chainable = false;\n"
        "try {\n"
        "  const client = new turbo.net.TcpClient();\n"
        "  const result = client.on('data', () => {});\n"
        "  var chainable = (result === client);\n"
        "} catch (e) {\n"
        "  var chainable = false;\n"
        "}\n");

    TEST_ASSERT_TRUE(get_bool_global("chainable"));
}

void test_tcp_client_connect_returns_promise(void) {
    js_turbo_test_eval(&env,
        "var isPromise = false;\n"
        "try {\n"
        "  const client = new turbo.net.TcpClient();\n"
        "  const promise = client.connect('127.0.0.1', 12345);\n"
        "  var isPromise = promise instanceof Promise;\n"
        "} catch (e) {\n"
        "  var isPromise = false;\n"
        "}\n");

    TEST_ASSERT_TRUE(get_bool_global("isPromise"));
}

void test_websocket_constructor(void) {
    js_turbo_test_eval(&env,
        "var wsCreated = false;\n"
        "var hasReady = false;\n"
        "try {\n"
        "  const ws = new turbo.net.WebSocket('ws://localhost:9999/test');\n"
        "  var wsCreated = true;\n"
        "  var hasReady = 'ready' in ws;\n"
        "  var hasSend = typeof ws.send === 'function';\n"
        "  var hasClose = typeof ws.close === 'function';\n"
        "  var hasOn = typeof ws.on === 'function';\n"
        "} catch (e) {\n"
        "  var wsCreated = false;\n"
        "  var error = e.message;\n"
        "}\n");

    TEST_ASSERT_TRUE(get_bool_global("wsCreated"));
    TEST_ASSERT_TRUE(get_bool_global("hasReady"));
    TEST_ASSERT_TRUE(get_bool_global("hasSend"));
    TEST_ASSERT_TRUE(get_bool_global("hasClose"));
    TEST_ASSERT_TRUE(get_bool_global("hasOn"));
}

void test_websocket_ready_is_promise(void) {
    js_turbo_test_eval(&env,
        "var readyIsPromise = false;\n"
        "try {\n"
        "  const ws = new turbo.net.WebSocket('ws://localhost:9999/test');\n"
        "  var readyIsPromise = ws.ready instanceof Promise;\n"
        "} catch (e) {\n"
        "  var readyIsPromise = false;\n"
        "}\n");

    TEST_ASSERT_TRUE(get_bool_global("readyIsPromise"));
}

void test_websocket_on_returns_this(void) {
    js_turbo_test_eval(&env,
        "var chainable = false;\n"
        "try {\n"
        "  const ws = new turbo.net.WebSocket('ws://localhost:9999/test');\n"
        "  const result = ws.on('message', () => {});\n"
        "  var chainable = (result === ws);\n"
        "} catch (e) {\n"
        "  var chainable = false;\n"
        "}\n");

    TEST_ASSERT_TRUE(get_bool_global("chainable"));
}

void test_websocket_url_parsing_ws(void) {
    // Test that ws:// URLs are accepted
    js_turbo_test_eval(&env,
        "var wsAccepted = false;\n"
        "try {\n"
        "  const ws = new turbo.net.WebSocket('ws://example.com:8080/path');\n"
        "  var wsAccepted = true;\n"
        "} catch (e) {\n"
        "  var wsAccepted = false;\n"
        "}\n");

    TEST_ASSERT_TRUE(get_bool_global("wsAccepted"));
}

void test_websocket_url_parsing_wss(void) {
    // Test that wss:// URLs are accepted
    js_turbo_test_eval(&env,
        "var wssAccepted = false;\n"
        "try {\n"
        "  const ws = new turbo.net.WebSocket('wss://example.com/secure');\n"
        "  var wssAccepted = true;\n"
        "} catch (e) {\n"
        "  var wssAccepted = false;\n"
        "}\n");

    TEST_ASSERT_TRUE(get_bool_global("wssAccepted"));
}

void test_websocket_invalid_url_rejected(void) {
    js_turbo_test_eval(&env,
        "var rejected = false;\n"
        "try {\n"
        "  const ws = new turbo.net.WebSocket('http://example.com');\n"
        "  var rejected = false;\n"
        "} catch (e) {\n"
        "  var rejected = true;\n"
        "}\n");

    TEST_ASSERT_TRUE(get_bool_global("rejected"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_net_module_exists);
    RUN_TEST(test_tcp_client_constructor);
    RUN_TEST(test_tcp_client_on_returns_this);
    RUN_TEST(test_tcp_client_connect_returns_promise);
    RUN_TEST(test_websocket_constructor);
    RUN_TEST(test_websocket_ready_is_promise);
    RUN_TEST(test_websocket_on_returns_this);
    RUN_TEST(test_websocket_url_parsing_ws);
    RUN_TEST(test_websocket_url_parsing_wss);
    RUN_TEST(test_websocket_invalid_url_rejected);
    return UNITY_END();
}
