#include "test_uv_fixture.h"

#include <stdlib.h>
#include <string.h>

static int get_bool_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
    JSValue prop = js_uv_test_global_prop(env, name);
    int result = JS_ToBool(env->ctx, prop);
    JS_FreeValue(env->ctx, prop);
    return result;
}

static const char *get_string_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
    JSValue prop = js_uv_test_global_prop(env, name);
    if (JS_IsNull(prop) || JS_IsUndefined(prop)) {
        JS_FreeValue(env->ctx, prop);
        return NULL;
    }
    if (!JS_IsString(prop)) {
        JS_FreeValue(env->ctx, prop);
        return NULL;
    }
    size_t len = 0;
    const char *str = JS_ToCStringLen(env->ctx, &len, prop);
    char *copy = (char *)malloc(len + 1);
    if (copy) {
        memcpy(copy, str, len);
        copy[len] = '\0';
    }
    JS_FreeCString(env->ctx, str);
    JS_FreeValue(env->ctx, prop);
    return copy;
}

spec("js_uv_net") {
    it("should verify net module exists") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var hasNet = typeof turbo.net === 'object';\n"
            "var hasTcpClient = typeof turbo.net.TcpClient === 'function';\n"
            "var hasWebSocket = typeof turbo.net.WebSocket === 'function';\n");

        check_true(get_bool_global(__bdd_config__, &env, "hasNet"));
        check_true(get_bool_global(__bdd_config__, &env, "hasTcpClient"));
        check_true(get_bool_global(__bdd_config__, &env, "hasWebSocket"));

        js_uv_test_env_cleanup(&env);
    }

    it("should verify tcp client constructor") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
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

        check_true(get_bool_global(__bdd_config__, &env, "clientCreated"));

        const char *type = get_string_global(__bdd_config__, &env, "clientType");
        check_str_eq("object", type);
        free((void *)type);

        check_true(get_bool_global(__bdd_config__, &env, "hasConnect"));
        check_true(get_bool_global(__bdd_config__, &env, "hasSend"));
        check_true(get_bool_global(__bdd_config__, &env, "hasClose"));
        check_true(get_bool_global(__bdd_config__, &env, "hasOn"));

        js_uv_test_env_cleanup(&env);
    }

    it("should verify tcp client on returns this") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var chainable = false;\n"
            "try {\n"
            "  const client = new turbo.net.TcpClient();\n"
            "  const result = client.on('data', () => {});\n"
            "  var chainable = (result === client);\n"
            "} catch (e) {\n"
            "  var chainable = false;\n"
            "}\n");

        check_true(get_bool_global(__bdd_config__, &env, "chainable"));

        js_uv_test_env_cleanup(&env);
    }

    it("should verify tcp client connect returns promise") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var isPromise = false;\n"
            "try {\n"
            "  const client = new turbo.net.TcpClient();\n"
            "  const promise = client.connect('127.0.0.1', 12345);\n"
            "  var isPromise = promise instanceof Promise;\n"
            "} catch (e) {\n"
            "  var isPromise = false;\n"
            "}\n");

        check_true(get_bool_global(__bdd_config__, &env, "isPromise"));

        js_uv_test_env_cleanup(&env);
    }

    it("should verify websocket constructor") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
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

        check_true(get_bool_global(__bdd_config__, &env, "wsCreated"));
        check_true(get_bool_global(__bdd_config__, &env, "hasReady"));
        check_true(get_bool_global(__bdd_config__, &env, "hasSend"));
        check_true(get_bool_global(__bdd_config__, &env, "hasClose"));
        check_true(get_bool_global(__bdd_config__, &env, "hasOn"));

        js_uv_test_env_cleanup(&env);
    }

    it("should verify websocket ready is promise") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var readyIsPromise = false;\n"
            "try {\n"
            "  const ws = new turbo.net.WebSocket('ws://localhost:9999/test');\n"
            "  var readyIsPromise = ws.ready instanceof Promise;\n"
            "} catch (e) {\n"
            "  var readyIsPromise = false;\n"
            "}\n");

        check_true(get_bool_global(__bdd_config__, &env, "readyIsPromise"));

        js_uv_test_env_cleanup(&env);
    }

    it("should verify websocket on returns this") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var chainable = false;\n"
            "try {\n"
            "  const ws = new turbo.net.WebSocket('ws://localhost:9999/test');\n"
            "  const result = ws.on('message', () => {});\n"
            "  var chainable = (result === ws);\n"
            "} catch (e) {\n"
            "  var chainable = false;\n"
            "}\n");

        check_true(get_bool_global(__bdd_config__, &env, "chainable"));

        js_uv_test_env_cleanup(&env);
    }

    it("should verify websocket url parsing ws") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        // Test that ws:// URLs are accepted
        js_uv_test_eval(&env,
            "var wsAccepted = false;\n"
            "try {\n"
            "  const ws = new turbo.net.WebSocket('ws://example.com:8080/path');\n"
            "  var wsAccepted = true;\n"
            "} catch (e) {\n"
            "  var wsAccepted = false;\n"
            "}\n");

        check_true(get_bool_global(__bdd_config__, &env, "wsAccepted"));

        js_uv_test_env_cleanup(&env);
    }

    it("should verify websocket url parsing wss") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        // Test that wss:// URLs are accepted
        js_uv_test_eval(&env,
            "var wssAccepted = false;\n"
            "var ws = null;\n"
            "try {\n"
            "  ws = new turbo.net.WebSocket('wss://example.com/secure');\n"
            "  var wssAccepted = true;\n"
            "} catch (e) {\n"
            "  var wssAccepted = false;\n"
            "}\n");

        check_true(get_bool_global(__bdd_config__, &env, "wssAccepted"));

        // Explicitly close the WebSocket and process events before cleanup
        js_uv_test_eval(&env,
            "if (ws && typeof ws.close === 'function') {\n"
            "  try { ws.close(); } catch (e) {}\n"
            "}\n"
            "ws = null;\n");
        
        // Process any pending cleanup events
        for (int i = 0; i < 5; i++) {
            js_uv_test_run_loop(&env);
        }

        js_uv_test_env_cleanup(&env);
    }

    it("should verify websocket invalid url rejected") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
            "var rejected = false;\n"
            "try {\n"
            "  const ws = new turbo.net.WebSocket('http://example.com');\n"
            "  var rejected = false;\n"
            "} catch (e) {\n"
            "  var rejected = true;\n"
            "}\n");

        check_true(get_bool_global(__bdd_config__, &env, "rejected"));

        js_uv_test_env_cleanup(&env);
    }
}
