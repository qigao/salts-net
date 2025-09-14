#include <stdio.h>
#include <stdlib.h>
#include "quickjs.h"
#include "js_string_utils.h"

static void print_js_value(JSContext *ctx, JSValue val) {
    const char* str = JS_ToCString(ctx, val);
    if (str) {
        printf("%s\n", str);
        JS_FreeCString(ctx, str);
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
    
    printf("=== String Utils Module Demo ===\n\n");
    
    // Test slugify
    printf("1. Slugify Tests:\n");
    JSValue result = JS_Eval(ctx, "strUtils.slugify('Hello World! This is a Test')", -1, "<eval>", JS_EVAL_TYPE_GLOBAL);
    printf("   slugify('Hello World! This is a Test') = ");
    print_js_value(ctx, result);
    JS_FreeValue(ctx, result);
    
    // Test capitalize
    printf("\n2. Capitalize Tests:\n");
    result = JS_Eval(ctx, "strUtils.capitalize('hello world from javascript')", -1, "<eval>", JS_EVAL_TYPE_GLOBAL);
    printf("   capitalize('hello world from javascript') = ");
    print_js_value(ctx, result);
    JS_FreeValue(ctx, result);
    
    // Test truncate
    printf("\n3. Truncate Tests:\n");
    result = JS_Eval(ctx, "strUtils.truncate('This is a very long string that should be truncated', 20)", -1, "<eval>", JS_EVAL_TYPE_GLOBAL);
    printf("   truncate('This is a very long string...', 20) = ");
    print_js_value(ctx, result);
    JS_FreeValue(ctx, result);
    
    // Test hash
    printf("\n4. Hash Tests:\n");
    result = JS_Eval(ctx, "strUtils.hash('test string')", -1, "<eval>", JS_EVAL_TYPE_GLOBAL);
    printf("   hash('test string') = ");
    print_js_value(ctx, result);
    JS_FreeValue(ctx, result);
    
    // Test template
    printf("\n5. Template Tests:\n");
    const char* template_code = 
        "strUtils.template('Hello {{name}}, you have {{count}} messages', "
        "{name: 'Alice', count: 5})";
    result = JS_Eval(ctx, template_code, -1, "<eval>", JS_EVAL_TYPE_GLOBAL);
    printf("   template('Hello {{name}}, you have {{count}} messages', {name: 'Alice', count: 5}) = ");
    print_js_value(ctx, result);
    JS_FreeValue(ctx, result);
    
    printf("\n=== Demo Complete ===\n");
    
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}