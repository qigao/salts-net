/**
 * @file js_os.c
 * @brief OS bindings for QuickJS using libuv
 */
#include "js_internal.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>

static JSValue js_os_hostname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char buf[256];
    size_t size = sizeof(buf);
    int rc = uv_os_gethostname(buf, &size);
    if (rc != 0) {
        return JS_ThrowInternalError(ctx, "Failed to get hostname: %s", uv_strerror(rc));
    }
    return JS_NewString(ctx, buf);
}

static JSValue js_os_homedir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char buf[1024];
    size_t size = sizeof(buf);
    int rc = uv_os_homedir(buf, &size);
    if (rc != 0) {
        return JS_ThrowInternalError(ctx, "Failed to get homedir: %s", uv_strerror(rc));
    }
    return JS_NewString(ctx, buf);
}

static JSValue js_os_tmpdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char buf[1024];
    size_t size = sizeof(buf);
    int rc = uv_os_tmpdir(buf, &size);
    if (rc != 0) {
        return JS_ThrowInternalError(ctx, "Failed to get tmpdir: %s", uv_strerror(rc));
    }
    return JS_NewString(ctx, buf);
}

static JSValue js_os_uptime(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    double uptime;
    int rc = uv_uptime(&uptime);
    if (rc != 0) {
        return JS_ThrowInternalError(ctx, "Failed to get uptime: %s", uv_strerror(rc));
    }
    return JS_NewFloat64(ctx, uptime);
}

static JSValue js_os_loadavg(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    double avg[3];
    uv_loadavg(avg);
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, avg[0]));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, avg[1]));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, avg[2]));
    return arr;
}

static JSValue js_os_cwd(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char buf[1024];
    size_t size = sizeof(buf);
    int rc = uv_cwd(buf, &size);
    if (rc != 0) {
        return JS_ThrowInternalError(ctx, "Failed to get cwd: %s", uv_strerror(rc));
    }
    return JS_NewString(ctx, buf);
}

static JSValue js_os_chdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "chdir requires path argument");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    int rc = uv_chdir(path);
    JS_FreeCString(ctx, path);
    if (rc != 0) {
        return JS_ThrowInternalError(ctx, "Failed to chdir: %s", uv_strerror(rc));
    }
    return JS_UNDEFINED;
}

static JSValue js_os_getenv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "getenv requires name argument");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    char buf[4096];
    size_t size = sizeof(buf);
    int rc = uv_os_getenv(name, buf, &size);
    JS_FreeCString(ctx, name);
    if (rc == UV_ENOENT) {
        return JS_UNDEFINED;
    }
    if (rc != 0) {
        return JS_ThrowInternalError(ctx, "Failed to getenv: %s", uv_strerror(rc));
    }
    return JS_NewString(ctx, buf);
}

static JSValue js_os_setenv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "setenv requires name and value arguments");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    const char *value = JS_ToCString(ctx, argv[1]);
    if (!value) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    int rc = uv_os_setenv(name, value);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    if (rc != 0) {
        return JS_ThrowInternalError(ctx, "Failed to setenv: %s", uv_strerror(rc));
    }
    return JS_UNDEFINED;
}

static JSValue js_os_unsetenv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "unsetenv requires name argument");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    int rc = uv_os_unsetenv(name);
    JS_FreeCString(ctx, name);
    if (rc != 0) {
        return JS_ThrowInternalError(ctx, "Failed to unsetenv: %s", uv_strerror(rc));
    }
    return JS_UNDEFINED;
}

static JSValue js_os_pid(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, uv_os_getpid());
}

static JSValue js_os_ppid(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, uv_os_getppid());
}

static const JSCFunctionListEntry js_os_funcs[] = {
    JS_CFUNC_DEF("hostname", 0, js_os_hostname),
    JS_CFUNC_DEF("homedir", 0, js_os_homedir),
    JS_CFUNC_DEF("tmpdir", 0, js_os_tmpdir),
    JS_CFUNC_DEF("uptime", 0, js_os_uptime),
    JS_CFUNC_DEF("loadavg", 0, js_os_loadavg),
    JS_CFUNC_DEF("cwd", 0, js_os_cwd),
    JS_CFUNC_DEF("chdir", 1, js_os_chdir),
    JS_CFUNC_DEF("getenv", 1, js_os_getenv),
    JS_CFUNC_DEF("setenv", 2, js_os_setenv),
    JS_CFUNC_DEF("unsetenv", 1, js_os_unsetenv),
    JS_CFUNC_DEF("pid", 0, js_os_pid),
    JS_CFUNC_DEF("ppid", 0, js_os_ppid),
};

int js_turbo_register_os(JSContext *ctx, JSValue turbo_obj) {
    JSValue os_obj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, os_obj, js_os_funcs, countof(js_os_funcs));
    JS_SetPropertyStr(ctx, turbo_obj, "os", os_obj);
    return 0;
}
