#include <stdio.h>
#include <stdlib.h>
#include "quickjs.h"
#include "js_string_utils.h"

static void print_js_value(JSContext *ctx, JSValue val) {
    if (JS_IsException(val)) {
        printf("EXCEPTION\n");
        return;
    }
    
    const char* str = JS_ToCString(ctx, val);
    if (str) {
        printf("'%s'\n", str);
        JS_FreeCString(ctx, str);
    } else {
        printf("NULL\n");
    }
}

int main() {
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) {
        fprintf(stderr, "Failed to create JS runtime\n");
        return 1;
    }
    
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        fprintf(stderr, "Failed to create JS context\n");
        JS_FreeRuntime(rt);
        return 1;
    }
    
    // Initialize our string utils module
    if (js_init_string_utils_module(ctx) < 0) {
        fprintf(stderr, "Failed to initialize string utils module\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }
    
    printf("=== Debug String Utils ===\n");
    
    // Test simple string
    printf("Test 1: ");
    JSValue result = JS_Eval(ctx, "strUtils.slugify('hello')", -1, "<eval>", JS_EVAL_TYPE_GLOBAL);
    print_js_value(ctx, result);
    JS_FreeValue(ctx, result);
    
    // Test with spaces
    printf("Test 2: ");
    result = JS_Eval(ctx, "strUtils.slugify('hello world')", -1, "<eval>", JS_EVAL_TYPE_GLOBAL);
    print_js_value(ctx, result);
    JS_FreeValue(ctx, result);
    
    // Test capitalize
    printf("Test 3: ");
    result = JS_Eval(ctx, "strUtils.capitalize('hello world')", -1, "<eval>", JS_EVAL_TYPE_GLOBAL);
    print_js_value(ctx, result);
    JS_FreeValue(ctx, result);
    
    // Test if strUtils exists
    printf("Test 4: ");
    result = JS_Eval(ctx, "typeof strUtils", -1, "<eval>", JS_EVAL_TYPE_GLOBAL);
    print_js_value(ctx, result);
    JS_FreeValue(ctx, result);
    
    // Test if slugify exists
    printf("Test 5: ");
    result = JS_Eval(ctx, "typeof strUtils.slugify", -1, "<eval>", JS_EVAL_TYPE_GLOBAL);
    print_js_value(ctx, result);
    JS_FreeValue(ctx, result);
    
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}