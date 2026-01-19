/**
 * @file js_module_loader.c
 * @brief ES6 module loader for TurboNet modules
 *
 * Supports:
 *   import dns from 'turbo:dns'
 *   import { resolve, resolveAsync } from 'turbo:dns'
 *   import fs from 'turbo:fs'
 *   import http from 'turbo:http'
 *   import timers from 'turbo:timers'
 *   import utils from 'turbo:utils'
 *   import net from 'turbo:net'
 *   import os from 'turbo:os'
 *   import signal from 'turbo:signal'
 *   import proc from 'turbo:proc'
 */
#include "js_internal.h"
#include "tlog.h"
#include <string.h>

// Forward declarations for module init functions
static JSModuleDef *js_init_module_dns(JSContext *ctx, const char *module_name);
static JSModuleDef *js_init_module_fs(JSContext *ctx, const char *module_name);
static JSModuleDef *js_init_module_http(JSContext *ctx, const char *module_name);
static JSModuleDef *js_init_module_timers(JSContext *ctx, const char *module_name);
static JSModuleDef *js_init_module_utils(JSContext *ctx, const char *module_name);
static JSModuleDef *js_init_module_net(JSContext *ctx, const char *module_name);
static JSModuleDef *js_init_module_os(JSContext *ctx, const char *module_name);
static JSModuleDef *js_init_module_signal(JSContext *ctx, const char *module_name);
static JSModuleDef *js_init_module_proc(JSContext *ctx, const char *module_name);

// Module registry
typedef struct {
    const char *name;
    JSModuleDef *(*init_func)(JSContext *ctx, const char *module_name);
} turbo_module_entry_t;

static const turbo_module_entry_t turbo_modules[] = {
    {"turbo:dns", js_init_module_dns},
    {"turbo:fs", js_init_module_fs},
    {"turbo:http", js_init_module_http},
    {"turbo:timers", js_init_module_timers},
    {"turbo:utils", js_init_module_utils},
    {"turbo:net", js_init_module_net},
    {"turbo:os", js_init_module_os},
    {"turbo:signal", js_init_module_signal},
    {"turbo:proc", js_init_module_proc},
    {NULL, NULL}
};

// =============================================================================
// DNS Module
// =============================================================================

extern JSValue js_dns_resolve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_dns_resolve_async(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_dns_set_servers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_dns_get_servers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

static int js_dns_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue resolve_fn = JS_NewCFunction(ctx, js_dns_resolve, "resolve", 2);
    JSValue resolve_async_fn = JS_NewCFunction(ctx, js_dns_resolve_async, "resolveAsync", 2);
    JSValue set_servers_fn = JS_NewCFunction(ctx, js_dns_set_servers, "setServers", 1);
    JSValue get_servers_fn = JS_NewCFunction(ctx, js_dns_get_servers, "getServers", 0);

    // Default export object
    JSValue default_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, default_obj, "resolve", JS_DupValue(ctx, resolve_fn));
    JS_SetPropertyStr(ctx, default_obj, "resolveAsync", JS_DupValue(ctx, resolve_async_fn));
    JS_SetPropertyStr(ctx, default_obj, "setServers", JS_DupValue(ctx, set_servers_fn));
    JS_SetPropertyStr(ctx, default_obj, "getServers", JS_DupValue(ctx, get_servers_fn));

    JS_SetModuleExport(ctx, m, "default", default_obj);
    JS_SetModuleExport(ctx, m, "resolve", resolve_fn);
    JS_SetModuleExport(ctx, m, "resolveAsync", resolve_async_fn);
    JS_SetModuleExport(ctx, m, "setServers", set_servers_fn);
    JS_SetModuleExport(ctx, m, "getServers", get_servers_fn);

    return 0;
}

static JSModuleDef *js_init_module_dns(JSContext *ctx, const char *module_name) {
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_dns_module_init);
    if (!m) return NULL;

    JS_AddModuleExport(ctx, m, "default");
    JS_AddModuleExport(ctx, m, "resolve");
    JS_AddModuleExport(ctx, m, "resolveAsync");
    JS_AddModuleExport(ctx, m, "setServers");
    JS_AddModuleExport(ctx, m, "getServers");

    return m;
}

// =============================================================================
// FS Module
// =============================================================================

extern JSValue js_fs_read_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_fs_write_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_fs_read_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_fs_write_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_fs_stat(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_fs_readdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_fs_mkdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_fs_join(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

static int js_fs_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue read_file_fn = JS_NewCFunction(ctx, js_fs_read_file, "readFile", 1);
    JSValue write_file_fn = JS_NewCFunction(ctx, js_fs_write_file, "writeFile", 2);
    JSValue read_json_fn = JS_NewCFunction(ctx, js_fs_read_json, "readJson", 1);
    JSValue write_json_fn = JS_NewCFunction(ctx, js_fs_write_json, "writeJson", 2);
    JSValue stat_fn = JS_NewCFunction(ctx, js_fs_stat, "stat", 1);
    JSValue readdir_fn = JS_NewCFunction(ctx, js_fs_readdir, "readdir", 1);
    JSValue mkdir_fn = JS_NewCFunction(ctx, js_fs_mkdir, "mkdir", 1);
    JSValue join_fn = JS_NewCFunction(ctx, js_fs_join, "join", 0);

    // Default export object
    JSValue default_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, default_obj, "readFile", JS_DupValue(ctx, read_file_fn));
    JS_SetPropertyStr(ctx, default_obj, "writeFile", JS_DupValue(ctx, write_file_fn));
    JS_SetPropertyStr(ctx, default_obj, "readJson", JS_DupValue(ctx, read_json_fn));
    JS_SetPropertyStr(ctx, default_obj, "writeJson", JS_DupValue(ctx, write_json_fn));
    JS_SetPropertyStr(ctx, default_obj, "stat", JS_DupValue(ctx, stat_fn));
    JS_SetPropertyStr(ctx, default_obj, "readdir", JS_DupValue(ctx, readdir_fn));
    JS_SetPropertyStr(ctx, default_obj, "mkdir", JS_DupValue(ctx, mkdir_fn));
    JS_SetPropertyStr(ctx, default_obj, "join", JS_DupValue(ctx, join_fn));

    JS_SetModuleExport(ctx, m, "default", default_obj);
    JS_SetModuleExport(ctx, m, "readFile", read_file_fn);
    JS_SetModuleExport(ctx, m, "writeFile", write_file_fn);
    JS_SetModuleExport(ctx, m, "readJson", read_json_fn);
    JS_SetModuleExport(ctx, m, "writeJson", write_json_fn);
    JS_SetModuleExport(ctx, m, "stat", stat_fn);
    JS_SetModuleExport(ctx, m, "readdir", readdir_fn);
    JS_SetModuleExport(ctx, m, "mkdir", mkdir_fn);
    JS_SetModuleExport(ctx, m, "join", join_fn);

    return 0;
}

static JSModuleDef *js_init_module_fs(JSContext *ctx, const char *module_name) {
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_fs_module_init);
    if (!m) return NULL;

    JS_AddModuleExport(ctx, m, "default");
    JS_AddModuleExport(ctx, m, "readFile");
    JS_AddModuleExport(ctx, m, "writeFile");
    JS_AddModuleExport(ctx, m, "readJson");
    JS_AddModuleExport(ctx, m, "writeJson");
    JS_AddModuleExport(ctx, m, "stat");
    JS_AddModuleExport(ctx, m, "readdir");
    JS_AddModuleExport(ctx, m, "mkdir");
    JS_AddModuleExport(ctx, m, "join");

    return m;
}

// =============================================================================
// HTTP Module
// =============================================================================

extern JSValue js_http_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_http_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_http_request(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

static int js_http_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue get_fn = JS_NewCFunction(ctx, js_http_get, "get", 2);
    JSValue post_fn = JS_NewCFunction(ctx, js_http_post, "post", 3);
    JSValue request_fn = JS_NewCFunction(ctx, js_http_request, "request", 3);

    // Default export object
    JSValue default_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, default_obj, "get", JS_DupValue(ctx, get_fn));
    JS_SetPropertyStr(ctx, default_obj, "post", JS_DupValue(ctx, post_fn));
    JS_SetPropertyStr(ctx, default_obj, "request", JS_DupValue(ctx, request_fn));

    JS_SetModuleExport(ctx, m, "default", default_obj);
    JS_SetModuleExport(ctx, m, "get", get_fn);
    JS_SetModuleExport(ctx, m, "post", post_fn);
    JS_SetModuleExport(ctx, m, "request", request_fn);

    return 0;
}

static JSModuleDef *js_init_module_http(JSContext *ctx, const char *module_name) {
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_http_module_init);
    if (!m) return NULL;

    JS_AddModuleExport(ctx, m, "default");
    JS_AddModuleExport(ctx, m, "get");
    JS_AddModuleExport(ctx, m, "post");
    JS_AddModuleExport(ctx, m, "request");

    return m;
}

// =============================================================================
// Timers Module
// =============================================================================

extern JSValue js_set_timeout(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_set_interval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_clear_timeout(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_clear_interval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_sleep(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

static int js_timers_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue set_timeout_fn = JS_NewCFunction(ctx, js_set_timeout, "setTimeout", 2);
    JSValue set_interval_fn = JS_NewCFunction(ctx, js_set_interval, "setInterval", 2);
    JSValue clear_timeout_fn = JS_NewCFunction(ctx, js_clear_timeout, "clearTimeout", 1);
    JSValue clear_interval_fn = JS_NewCFunction(ctx, js_clear_interval, "clearInterval", 1);
    JSValue sleep_fn = JS_NewCFunction(ctx, js_sleep, "sleep", 1);

    // Default export object
    JSValue default_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, default_obj, "setTimeout", JS_DupValue(ctx, set_timeout_fn));
    JS_SetPropertyStr(ctx, default_obj, "setInterval", JS_DupValue(ctx, set_interval_fn));
    JS_SetPropertyStr(ctx, default_obj, "clearTimeout", JS_DupValue(ctx, clear_timeout_fn));
    JS_SetPropertyStr(ctx, default_obj, "clearInterval", JS_DupValue(ctx, clear_interval_fn));
    JS_SetPropertyStr(ctx, default_obj, "sleep", JS_DupValue(ctx, sleep_fn));

    JS_SetModuleExport(ctx, m, "default", default_obj);
    JS_SetModuleExport(ctx, m, "setTimeout", set_timeout_fn);
    JS_SetModuleExport(ctx, m, "setInterval", set_interval_fn);
    JS_SetModuleExport(ctx, m, "clearTimeout", clear_timeout_fn);
    JS_SetModuleExport(ctx, m, "clearInterval", clear_interval_fn);
    JS_SetModuleExport(ctx, m, "sleep", sleep_fn);

    return 0;
}

static JSModuleDef *js_init_module_timers(JSContext *ctx, const char *module_name) {
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_timers_module_init);
    if (!m) return NULL;

    JS_AddModuleExport(ctx, m, "default");
    JS_AddModuleExport(ctx, m, "setTimeout");
    JS_AddModuleExport(ctx, m, "setInterval");
    JS_AddModuleExport(ctx, m, "clearTimeout");
    JS_AddModuleExport(ctx, m, "clearInterval");
    JS_AddModuleExport(ctx, m, "sleep");

    return m;
}

// =============================================================================
// Utils Module
// =============================================================================

extern JSValue js_base64_encode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
extern JSValue js_base64_decode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

static int js_utils_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue base64_encode_fn = JS_NewCFunction(ctx, js_base64_encode, "base64Encode", 1);
    JSValue base64_decode_fn = JS_NewCFunction(ctx, js_base64_decode, "base64Decode", 2);

    // Default export object
    JSValue default_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, default_obj, "base64Encode", JS_DupValue(ctx, base64_encode_fn));
    JS_SetPropertyStr(ctx, default_obj, "base64Decode", JS_DupValue(ctx, base64_decode_fn));

    JS_SetModuleExport(ctx, m, "default", default_obj);
    JS_SetModuleExport(ctx, m, "base64Encode", base64_encode_fn);
    JS_SetModuleExport(ctx, m, "base64Decode", base64_decode_fn);

    return 0;
}

static JSModuleDef *js_init_module_utils(JSContext *ctx, const char *module_name) {
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_utils_module_init);
    if (!m) return NULL;

    JS_AddModuleExport(ctx, m, "default");
    JS_AddModuleExport(ctx, m, "base64Encode");
    JS_AddModuleExport(ctx, m, "base64Decode");

    return m;
}

// =============================================================================
// Net Module
// =============================================================================

extern int js_turbo_register_net(JSContext *ctx, JSValue turbo_obj);

static int js_net_module_init(JSContext *ctx, JSModuleDef *m) {
    // Create net object with TcpClient and WebSocket classes
    JSValue net_obj = JS_NewObject(ctx);
    js_turbo_register_net(ctx, net_obj);

    // Get the net sub-object
    JSValue net_sub = JS_GetPropertyStr(ctx, net_obj, "net");

    // Export TcpClient and WebSocket
    JSValue tcp_client = JS_GetPropertyStr(ctx, net_sub, "TcpClient");
    JSValue websocket = JS_GetPropertyStr(ctx, net_sub, "WebSocket");

    JS_SetModuleExport(ctx, m, "default", net_sub);
    JS_SetModuleExport(ctx, m, "TcpClient", tcp_client);
    JS_SetModuleExport(ctx, m, "WebSocket", websocket);

    JS_FreeValue(ctx, net_obj);

    return 0;
}

static JSModuleDef *js_init_module_net(JSContext *ctx, const char *module_name) {
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_net_module_init);
    if (!m) return NULL;

    JS_AddModuleExport(ctx, m, "default");
    JS_AddModuleExport(ctx, m, "TcpClient");
    JS_AddModuleExport(ctx, m, "WebSocket");

    return m;
}

// =============================================================================
// OS Module
// =============================================================================

extern int js_turbo_register_os(JSContext *ctx, JSValue turbo_obj);

static int js_os_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue container = JS_NewObject(ctx);
    js_turbo_register_os(ctx, container);
    JSValue os_obj = JS_GetPropertyStr(ctx, container, "os");

    JS_SetModuleExport(ctx, m, "default", os_obj);
    JS_SetModuleExport(ctx, m, "hostname", JS_GetPropertyStr(ctx, os_obj, "hostname"));
    JS_SetModuleExport(ctx, m, "homedir", JS_GetPropertyStr(ctx, os_obj, "homedir"));
    JS_SetModuleExport(ctx, m, "tmpdir", JS_GetPropertyStr(ctx, os_obj, "tmpdir"));
    JS_SetModuleExport(ctx, m, "uptime", JS_GetPropertyStr(ctx, os_obj, "uptime"));
    JS_SetModuleExport(ctx, m, "loadavg", JS_GetPropertyStr(ctx, os_obj, "loadavg"));
    JS_SetModuleExport(ctx, m, "cwd", JS_GetPropertyStr(ctx, os_obj, "cwd"));
    JS_SetModuleExport(ctx, m, "chdir", JS_GetPropertyStr(ctx, os_obj, "chdir"));
    JS_SetModuleExport(ctx, m, "getenv", JS_GetPropertyStr(ctx, os_obj, "getenv"));
    JS_SetModuleExport(ctx, m, "setenv", JS_GetPropertyStr(ctx, os_obj, "setenv"));
    JS_SetModuleExport(ctx, m, "unsetenv", JS_GetPropertyStr(ctx, os_obj, "unsetenv"));
    JS_SetModuleExport(ctx, m, "pid", JS_GetPropertyStr(ctx, os_obj, "pid"));
    JS_SetModuleExport(ctx, m, "ppid", JS_GetPropertyStr(ctx, os_obj, "ppid"));

    JS_FreeValue(ctx, container);
    return 0;
}

static JSModuleDef *js_init_module_os(JSContext *ctx, const char *module_name) {
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_os_module_init);
    if (!m) return NULL;

    JS_AddModuleExport(ctx, m, "default");
    JS_AddModuleExport(ctx, m, "hostname");
    JS_AddModuleExport(ctx, m, "homedir");
    JS_AddModuleExport(ctx, m, "tmpdir");
    JS_AddModuleExport(ctx, m, "uptime");
    JS_AddModuleExport(ctx, m, "loadavg");
    JS_AddModuleExport(ctx, m, "cwd");
    JS_AddModuleExport(ctx, m, "chdir");
    JS_AddModuleExport(ctx, m, "getenv");
    JS_AddModuleExport(ctx, m, "setenv");
    JS_AddModuleExport(ctx, m, "unsetenv");
    JS_AddModuleExport(ctx, m, "pid");
    JS_AddModuleExport(ctx, m, "ppid");

    return m;
}

// =============================================================================
// Signal Module
// =============================================================================

extern int js_turbo_register_signal(JSContext *ctx, JSValue turbo_obj);

static int js_signal_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue container = JS_NewObject(ctx);
    js_turbo_register_signal(ctx, container);
    JSValue signal_obj = JS_GetPropertyStr(ctx, container, "signal");

    JS_SetModuleExport(ctx, m, "default", signal_obj);
    JS_SetModuleExport(ctx, m, "watch", JS_GetPropertyStr(ctx, signal_obj, "watch"));
    JS_SetModuleExport(ctx, m, "SIGINT", JS_GetPropertyStr(ctx, signal_obj, "SIGINT"));
    JS_SetModuleExport(ctx, m, "SIGTERM", JS_GetPropertyStr(ctx, signal_obj, "SIGTERM"));

    JS_FreeValue(ctx, container);
    return 0;
}

static JSModuleDef *js_init_module_signal(JSContext *ctx, const char *module_name) {
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_signal_module_init);
    if (!m) return NULL;

    JS_AddModuleExport(ctx, m, "default");
    JS_AddModuleExport(ctx, m, "watch");
    JS_AddModuleExport(ctx, m, "SIGINT");
    JS_AddModuleExport(ctx, m, "SIGTERM");

    return m;
}

// =============================================================================
// Proc Module
// =============================================================================

extern int js_turbo_register_proc(JSContext *ctx, JSValue turbo_obj);

static int js_proc_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue container = JS_NewObject(ctx);
    js_turbo_register_proc(ctx, container);
    JSValue proc_obj = JS_GetPropertyStr(ctx, container, "proc");

    JS_SetModuleExport(ctx, m, "default", proc_obj);
    JS_SetModuleExport(ctx, m, "spawn", JS_GetPropertyStr(ctx, proc_obj, "spawn"));
    JS_SetModuleExport(ctx, m, "kill", JS_GetPropertyStr(ctx, proc_obj, "kill"));

    JS_FreeValue(ctx, container);
    return 0;
}

static JSModuleDef *js_init_module_proc(JSContext *ctx, const char *module_name) {
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_proc_module_init);
    if (!m) return NULL;

    JS_AddModuleExport(ctx, m, "default");
    JS_AddModuleExport(ctx, m, "spawn");
    JS_AddModuleExport(ctx, m, "kill");

    return m;
}

// =============================================================================
// Module Loader
// =============================================================================

// Read file contents for module loading
static char *js_load_file(JSContext *ctx, const char *filename, size_t *out_len) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len < 0) {
        fclose(f);
        return NULL;
    }

    char *buf = js_malloc(ctx, len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read_len = fread(buf, 1, len, f);
    fclose(f);

    if (read_len != (size_t)len) {
        js_free(ctx, buf);
        return NULL;
    }

    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

// Load a JS module from file
static JSModuleDef *js_load_module_file(JSContext *ctx, const char *module_name) {
    size_t buf_len;
    char *buf = js_load_file(ctx, module_name, &buf_len);
    if (!buf) {
        JS_ThrowReferenceError(ctx, "could not load module file '%s'", module_name);
        return NULL;
    }

    // Compile the module
    JSValue func_val = JS_Eval(ctx, buf, buf_len, module_name,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    js_free(ctx, buf);

    if (JS_IsException(func_val)) {
        return NULL;
    }

    // Get the module definition - must not free func_val as QuickJS takes ownership
    JSModuleDef *m = JS_VALUE_GET_PTR(func_val);
    return m;
}

static JSModuleDef *js_turbo_module_loader(JSContext *ctx, const char *module_name, void *opaque) {
    (void)opaque;

    // Check if it's a turbo module
    for (int i = 0; turbo_modules[i].name != NULL; i++) {
        if (strcmp(module_name, turbo_modules[i].name) == 0) {
            return turbo_modules[i].init_func(ctx, module_name);
        }
    }

    // Try to load from file system
    // Check for .js extension or add it
    const char *ext = strrchr(module_name, '.');
    if (ext && strcmp(ext, ".js") == 0) {
        return js_load_module_file(ctx, module_name);
    }

    // Try with .js extension
    size_t name_len = strlen(module_name);
    char *js_name = js_malloc(ctx, name_len + 4);
    if (js_name) {
        memcpy(js_name, module_name, name_len);
        memcpy(js_name + name_len, ".js", 4);
        JSModuleDef *m = js_load_module_file(ctx, js_name);
        js_free(ctx, js_name);
        if (m) return m;
    }

    // Try original name (might be a directory with index.js or other extension)
    return js_load_module_file(ctx, module_name);
}

// Normalize module name (for relative imports)
static char *js_turbo_module_normalize(JSContext *ctx, const char *base_name,
                                        const char *name, void *opaque) {
    (void)opaque;

    // For turbo: modules, return as-is
    if (strncmp(name, "turbo:", 6) == 0) {
        return js_strdup(ctx, name);
    }

    // For absolute paths, return as-is
    if (name[0] == '/' || (name[0] != '\0' && name[1] == ':')) {
        return js_strdup(ctx, name);
    }

    // For relative paths, resolve against base_name
    if (name[0] == '.' && base_name) {
        // Find the directory of base_name
        const char *last_slash = strrchr(base_name, '/');
        const char *last_backslash = strrchr(base_name, '\\');
        const char *dir_end = last_slash > last_backslash ? last_slash : last_backslash;

        if (dir_end) {
            size_t dir_len = dir_end - base_name + 1;
            size_t name_len = strlen(name);
            char *result = js_malloc(ctx, dir_len + name_len + 1);
            if (result) {
                memcpy(result, base_name, dir_len);
                memcpy(result + dir_len, name, name_len + 1);
                return result;
            }
        }
    }

    // For other modules, return as-is
    return js_strdup(ctx, name);
}

// =============================================================================
// Public API
// =============================================================================

void js_turbo_init_module_loader(JSContext *ctx) {
    JS_SetModuleLoaderFunc(JS_GetRuntime(ctx), js_turbo_module_normalize,
                           js_turbo_module_loader, NULL);
}
