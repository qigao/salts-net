/**
 * @file js_fs.c
 * @brief Unified File System bindings for QuickJS leveraging TurboCommon.
 */
#include "js_internal.h"
#include "turbo_fs.h"
#include "tlog.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

JSValue js_fs_stat(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    turbo_fs_stat_t st;
    int rc = turbo_fs_stat_sync(path, &st);
    JS_FreeCString(ctx, path);

    if (rc < 0) return JS_NULL;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, st.size));
    JS_SetPropertyStr(ctx, obj, "mode", JS_NewInt32(ctx, st.mode));
    JS_SetPropertyStr(ctx, obj, "isFile", JS_NewBool(ctx, st.is_file));
    JS_SetPropertyStr(ctx, obj, "isDirectory", JS_NewBool(ctx, st.is_directory));
    JS_SetPropertyStr(ctx, obj, "mtime", JS_NewInt64(ctx, st.mtime));
    return obj;
}

JSValue js_fs_read_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    turbo_fs_buf_t buf;
    int rc = turbo_fs_read_file_sync(path, &buf);
    JS_FreeCString(ctx, path);

    if (rc < 0) return JS_ThrowReferenceError(ctx, "Could not open file");

    JSValue res;
    bool binary = false;
    if (argc > 1 && JS_IsString(argv[1])) {
        const char *mode = JS_ToCString(ctx, argv[1]);
        if (mode && strcmp(mode, "binary") == 0) binary = true;
        JS_FreeCString(ctx, mode);
    }

    if (binary) {
        res = JS_NewArrayBufferCopy(ctx, (uint8_t*)buf.base, buf.len);
    } else {
        res = JS_NewStringLen(ctx, buf.base, buf.len);
    }

    turbo_fs_buf_free(&buf);
    return res;
}

JSValue js_fs_write_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    uint8_t *data = NULL;
    size_t len = 0;
    if (js_turbo_collect_data(ctx, argv[1], &data, &len) != 0) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }

    turbo_fs_buf_t buf = { .base = (char*)data, .len = len };
    int rc = turbo_fs_write_file_sync(path, &buf);
    
    free(data);
    JS_FreeCString(ctx, path);

    return JS_NewBool(ctx, rc == 0);
}

JSValue js_fs_read_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue content = js_fs_read_file(ctx, this_val, argc, argv);
    if (JS_IsException(content) || JS_IsNull(content)) return content;

    size_t len;
    const char *str = JS_ToCStringLen(ctx, &len, content);
    JSValue res = JS_ParseJSON(ctx, str, len, "json");
    
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, content);
    return res;
}

JSValue js_fs_write_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    JSValue str = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(str)) return str;

    JSValue args[2] = { JS_DupValue(ctx, argv[0]), str };
    JSValue res = js_fs_write_file(ctx, this_val, 2, args);
    
    JS_FreeValue(ctx, args[0]);
    return res;
}

JSValue js_fs_readdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;

#ifdef _WIN32
    // Windows implementation
    char search_path[TURBO_FS_MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", path);
    
    WIN32_FIND_DATAA find_data;
    HANDLE hFind = FindFirstFileA(search_path, &find_data);
    
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            // Skip . and .. entries
            if (strcmp(find_data.cFileName, ".") != 0 && strcmp(find_data.cFileName, "..") != 0) {
                JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, find_data.cFileName));
            }
        } while (FindNextFileA(hFind, &find_data));
        FindClose(hFind);
    }
#else
    // Unix/Linux implementation
    #include <dirent.h>
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            // Skip . and .. entries
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, entry->d_name));
            }
        }
        closedir(dir);
    }
#endif

    JS_FreeCString(ctx, path);
    return arr;
}

JSValue js_fs_join(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char result[TURBO_FS_MAX_PATH] = {0};
    char current[TURBO_FS_MAX_PATH] = {0};
    
    if (argc > 0) {
        const char *base = JS_ToCString(ctx, argv[0]);
        strncpy(result, base, sizeof(result)-1);
        JS_FreeCString(ctx, base);
    }

    for (int i = 1; i < argc; i++) {
        const char *part = JS_ToCString(ctx, argv[i]);
        strncpy(current, result, sizeof(current)-1);
        turbo_fs_path_join(result, sizeof(result), current, part);
        JS_FreeCString(ctx, part);
    }

    return JS_NewString(ctx, result);
}

JSValue js_fs_mkdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    int mode = 0755;
    if (argc > 1) JS_ToInt32(ctx, &mode, argv[1]);

    int rc = turbo_fs_mkdir_sync(path, mode);
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, rc == 0);
}

static const JSCFunctionListEntry js_fs_funcs[] = {
    JS_CFUNC_DEF("stat", 1, js_fs_stat),
    JS_CFUNC_DEF("readFile", 2, js_fs_read_file),
    JS_CFUNC_DEF("writeFile", 2, js_fs_write_file),
    JS_CFUNC_DEF("readJson", 1, js_fs_read_json),
    JS_CFUNC_DEF("writeJson", 2, js_fs_write_json),
    JS_CFUNC_DEF("readdir", 1, js_fs_readdir),
    JS_CFUNC_DEF("mkdir", 2, js_fs_mkdir),
    JS_CFUNC_DEF("join", 1, js_fs_join),
};

int js_turbo_register_fs(JSContext *ctx, JSValue turbo_obj) {
    JSValue fs_obj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, fs_obj, js_fs_funcs, sizeof(js_fs_funcs)/sizeof(js_fs_funcs[0]));
    JS_SetPropertyStr(ctx, turbo_obj, "fs", fs_obj);
    return 0;
}
