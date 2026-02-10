#include "test_uv_fixture.h"

#include <stdlib.h>
#include <string.h>

spec("js_uv_multi_runtime") {
    it("should handle single runtime init and cleanup") {
        JSRuntime *rt = JS_NewRuntime();
        check_not_null(rt);

        JSContext *ctx = JS_NewContext(rt);
        check_not_null(ctx);

        int result = js_init_turbo_module(ctx);
        check_int_eq(0, result);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    it("should handle multiple sequential runtimes") {
        // Create and destroy multiple runtimes sequentially
        for (int i = 0; i < 3; i++) {
            JSRuntime *rt = JS_NewRuntime();
            check_not_null(rt);

            JSContext *ctx = JS_NewContext(rt);
            check_not_null(ctx);

            int result = js_init_turbo_module(ctx);
            check_int_eq(0, result);

            // Verify turbo object exists
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue turbo = JS_GetPropertyStr(ctx, global, "turbo");
            check_false(JS_IsUndefined(turbo));
            check_false(JS_IsException(turbo));

            JS_FreeValue(ctx, turbo);
            JS_FreeValue(ctx, global);

            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
        }
    }

    it("should handle multiple contexts same runtime") {
        JSRuntime *rt = JS_NewRuntime();
        check_not_null(rt);

        // Create multiple contexts on the same runtime
        for (int i = 0; i < 3; i++) {
            JSContext *ctx = JS_NewContext(rt);
            check_not_null(ctx);

            int result = js_init_turbo_module(ctx);
            check_int_eq(0, result);

            // Verify turbo object exists
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue turbo = JS_GetPropertyStr(ctx, global, "turbo");
            check_false(JS_IsUndefined(turbo));

            JS_FreeValue(ctx, turbo);
            JS_FreeValue(ctx, global);

            JS_FreeContext(ctx);
        }

        JS_FreeRuntime(rt);
    }

    it("should handle concurrent runtimes") {
        // Create multiple runtimes that exist at the same time
        JSRuntime *rt1 = JS_NewRuntime();
        JSRuntime *rt2 = JS_NewRuntime();
        check_not_null(rt1);
        check_not_null(rt2);

        JSContext *ctx1 = JS_NewContext(rt1);
        JSContext *ctx2 = JS_NewContext(rt2);
        check_not_null(ctx1);
        check_not_null(ctx2);

        // Initialize both
        check_int_eq(0, js_init_turbo_module(ctx1));
        check_int_eq(0, js_init_turbo_module(ctx2));

        // Verify both work
        JSValue global1 = JS_GetGlobalObject(ctx1);
        JSValue turbo1 = JS_GetPropertyStr(ctx1, global1, "turbo");
        check_false(JS_IsUndefined(turbo1));

        JSValue global2 = JS_GetGlobalObject(ctx2);
        JSValue turbo2 = JS_GetPropertyStr(ctx2, global2, "turbo");
        check_false(JS_IsUndefined(turbo2));

        JS_FreeValue(ctx1, turbo1);
        JS_FreeValue(ctx1, global1);
        JS_FreeValue(ctx2, turbo2);
        JS_FreeValue(ctx2, global2);

        // Clean up in different order than creation
        JS_FreeContext(ctx2);
        JS_FreeRuntime(rt2);
        JS_FreeContext(ctx1);
        JS_FreeRuntime(rt1);
    }

    it("should eval after init") {
        JSRuntime *rt = JS_NewRuntime();
        check_not_null(rt);

        JSContext *ctx = JS_NewContext(rt);
        check_not_null(ctx);

        check_int_eq(0, js_init_turbo_module(ctx));

        // Test basic eval
        const char *code = "var result = typeof turbo; result;";
        JSValue val = JS_Eval(ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_GLOBAL);
        check_false(JS_IsException(val));

        const char *str = JS_ToCString(ctx, val);
        check_not_null(str);
        check_str_eq("object", str);

        JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, val);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    it("should verify turbo submodules exist") {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        check_int_eq(0, js_init_turbo_module(ctx));

        const char *code =
            "var checks = {"
            "  hasFs: typeof turbo.fs === 'object',"
            "  hasDns: typeof turbo.dns === 'object',"
            "  hasHttp: typeof turbo.http === 'object',"
            "  hasOs: typeof turbo.os === 'object',"
            "  hasSignal: typeof turbo.signal === 'object',"
            "  hasProc: typeof turbo.proc === 'object',"
            "  hasNet: typeof turbo.net === 'object'"
            "};"
            "checks;";

        JSValue val = JS_Eval(ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_GLOBAL);
        check_false(JS_IsException(val));

        JSValue hasFs = JS_GetPropertyStr(ctx, val, "hasFs");
        JSValue hasDns = JS_GetPropertyStr(ctx, val, "hasDns");
        JSValue hasHttp = JS_GetPropertyStr(ctx, val, "hasHttp");
        JSValue hasOs = JS_GetPropertyStr(ctx, val, "hasOs");
        JSValue hasSignal = JS_GetPropertyStr(ctx, val, "hasSignal");
        JSValue hasProc = JS_GetPropertyStr(ctx, val, "hasProc");
        JSValue hasNet = JS_GetPropertyStr(ctx, val, "hasNet");

        check_true(JS_ToBool(ctx, hasFs));
        check_true(JS_ToBool(ctx, hasDns));
        check_true(JS_ToBool(ctx, hasHttp));
        check_true(JS_ToBool(ctx, hasOs));
        check_true(JS_ToBool(ctx, hasSignal));
        check_true(JS_ToBool(ctx, hasProc));
        check_true(JS_ToBool(ctx, hasNet));

        JS_FreeValue(ctx, hasFs);
        JS_FreeValue(ctx, hasDns);
        JS_FreeValue(ctx, hasHttp);
        JS_FreeValue(ctx, hasOs);
        JS_FreeValue(ctx, hasSignal);
        JS_FreeValue(ctx, hasProc);
        JS_FreeValue(ctx, hasNet);
        JS_FreeValue(ctx, val);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }
}
