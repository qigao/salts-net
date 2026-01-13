/**
 * @file js_utils.c
 * @brief Utility bindings for QuickJS (base64, etc.)
 */
#include "js_internal.h"
#include "base64_utils.h"
#include <stdlib.h>
#include <string.h>

JSValue js_base64_encode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "base64.encode requires data argument");

    uint8_t *data = NULL;
    size_t len = 0;

    // Handle string or ArrayBuffer
    if (JS_IsString(argv[0])) {
        const char *str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str) return JS_EXCEPTION;
        data = (uint8_t *)str;

        char *output = NULL;
        int rc = tn_base64_encode(data, len, &output);
        JS_FreeCString(ctx, str);

        if (rc != 0 || !output) {
            return JS_ThrowInternalError(ctx, "base64 encode failed");
        }

        JSValue result = JS_NewString(ctx, output);
        free(output);
        return result;
    } else {
        // ArrayBuffer or TypedArray
        size_t byte_len;
        uint8_t *buf = JS_GetArrayBuffer(ctx, &byte_len, argv[0]);
        if (!buf) {
            // Try TypedArray
            size_t offset, elem_size;
            JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &offset, &byte_len, &elem_size);
            if (JS_IsException(ab)) {
                return JS_ThrowTypeError(ctx, "base64.encode expects string or ArrayBuffer");
            }
            buf = JS_GetArrayBuffer(ctx, &byte_len, ab);
            JS_FreeValue(ctx, ab);
            if (!buf) {
                return JS_ThrowTypeError(ctx, "base64.encode expects string or ArrayBuffer");
            }
            buf += offset;
        }

        char *output = NULL;
        int rc = tn_base64_encode(buf, byte_len, &output);

        if (rc != 0 || !output) {
            return JS_ThrowInternalError(ctx, "base64 encode failed");
        }

        JSValue result = JS_NewString(ctx, output);
        free(output);
        return result;
    }
}

JSValue js_base64_decode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "base64.decode requires string argument");

    const char *input = JS_ToCString(ctx, argv[0]);
    if (!input) return JS_EXCEPTION;

    uint8_t *output = NULL;
    size_t output_len = 0;
    int rc = tn_base64_decode(input, &output, &output_len);
    JS_FreeCString(ctx, input);

    if (rc != 0 || !output) {
        return JS_ThrowInternalError(ctx, "base64 decode failed");
    }

    // Return as ArrayBuffer by default, or string if second arg is "string"
    bool as_string = false;
    if (argc > 1 && JS_IsString(argv[1])) {
        const char *mode = JS_ToCString(ctx, argv[1]);
        if (mode && strcmp(mode, "string") == 0) as_string = true;
        JS_FreeCString(ctx, mode);
    }

    JSValue result;
    if (as_string) {
        result = JS_NewStringLen(ctx, (char *)output, output_len);
    } else {
        result = JS_NewArrayBufferCopy(ctx, output, output_len);
    }

    free(output);
    return result;
}

static const JSCFunctionListEntry js_utils_funcs[] = {
    JS_CFUNC_DEF("base64Encode", 1, js_base64_encode),
    JS_CFUNC_DEF("base64Decode", 2, js_base64_decode),
};

int js_turbo_register_utils(JSContext *ctx, JSValue turbo_obj) {
    JS_SetPropertyFunctionList(ctx, turbo_obj, js_utils_funcs, countof(js_utils_funcs));
    return 0;
}
