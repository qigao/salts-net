#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "js_module.h"

static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            printf("%s%s", i > 0 ? " " : "", str);
            JS_FreeCString(ctx, str);
        }
    }
    printf("\n");
    return JS_UNDEFINED;
}

static char *read_file(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (buf) {
        fread(buf, 1, size, f);
        buf[size] = '\0';
    }
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    const char *filename = "http_example.js";
    if (argc > 1) filename = argv[1];

    char *script = read_file(filename);
    if (!script) {
        fprintf(stderr, "Could not read file: %s\n", filename);
        return 1;
    }

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    /* Setup console.log */
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_print, "log", 1));
    JS_SetPropertyStr(ctx, global_obj, "console", console);
    JS_FreeValue(ctx, global_obj);

    /* Init TurboNet Modules */
    js_init_turbo_module(ctx);

    JSValue val = JS_Eval(ctx, script, strlen(script), filename, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        JSValue exc = JS_GetException(ctx);
        const char *str = JS_ToCString(ctx, exc);
        fprintf(stderr, "Exception: %s\n", str);
        JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, val);

    /* Process events */
    js_turbo_process_events(ctx);

    free(script);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
