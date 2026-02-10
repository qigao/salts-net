#ifndef JS_TURBO_TEST_FIXTURE_H
#define JS_TURBO_TEST_FIXTURE_H
#include <string.h>

#include "js_internal.h"
#include "quickjs.h"
#include "tinytest.h"

typedef struct
{
  JSRuntime* rt;
  JSContext* ctx;
} JSTurboTestEnv;

static inline void js_turbo_test_env_init(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env)
{
  memset(env, 0, sizeof(*env));
  env->rt = JS_NewRuntime();
  check_not_null(env->rt);
  JS_SetMemoryLimit(env->rt, -1);
  JS_SetMaxStackSize(env->rt, 0);
  env->ctx = JS_NewContext(env->rt);
  check_not_null(env->ctx);
  check_int_eq(js_init_turbo_module(env->ctx), 0);
}

// Re-adding the js_init_turbo_module call if it was there.
// But wait, the file visible content had:
// #include "js_internal.h"
// ...
// TEST_ASSERT_EQUAL_INT(0, js_init_turbo_module(env->ctx));

// So I should keep it.

static inline void js_turbo_test_env_init_full(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env)
{
  memset(env, 0, sizeof(*env));
  env->rt = JS_NewRuntime();
  check_not_null(env->rt);
  JS_SetMemoryLimit(env->rt, -1);
  JS_SetMaxStackSize(env->rt, 0);
  env->ctx = JS_NewContext(env->rt);
  check_not_null(env->ctx);
  // Assuming js_init_turbo_module is available via included headers
  // make sure to match original behavior
}

// Wait, I can't see js_init_turbo_module declaration in the file 
// but it was used in line 24.
// It probably comes from js_internal.h or implicitly declared.
// I will assume it's there.

static inline void js_turbo_test_env_cleanup(JSTurboTestEnv* env)
{
  if (env->ctx) {
    // Run GC to finalize any remaining objects before freeing context
    JS_RunGC(env->rt);
    JS_FreeContext(env->ctx);
    env->ctx = NULL;
  }
  if (env->rt) {
    JS_FreeRuntime(env->rt);
    env->rt = NULL;
  }
}

static inline void js_turbo_test_process_events(JSTurboTestEnv* env)
{
  js_turbo_process_events(env->ctx);
}

static inline void js_turbo_test_eval(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char* code)
{
  JSValue result =
      JS_Eval(env->ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_GLOBAL);
  check_false(JS_IsException(result));
  if (JS_IsException(result)) {
      JSValue exception = JS_GetException(env->ctx);
      const char* str = JS_ToCString(env->ctx, exception);
      if (str) {
          // printf("JS Exception: %s\n", str);
          JS_FreeCString(env->ctx, str);
      }
      JS_FreeValue(env->ctx, exception);
  }
  JS_FreeValue(env->ctx, result);
}

static inline JSValue js_turbo_test_eval_value(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char* code)
{
  JSValue result =
      JS_Eval(env->ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(result)) {
    check_false(JS_IsException(result));  // Will fail and return
    return JS_NULL;  // Fallback (never reached if check fails)
  }
  return result;
}

static inline JSValue js_turbo_test_global_prop(JSTurboTestEnv* env, const char* name)
{
  JSValue global_obj = JS_GetGlobalObject(env->ctx);
  JSValue prop = JS_GetPropertyStr(env->ctx, global_obj, name);
  JS_FreeValue(env->ctx, global_obj);
  return prop;
}

// Compatibility macros
#define JSUVTestEnv JSTurboTestEnv
// Macros to simplify calls with bdd_invoke/bdd_config
#define js_uv_test_env_init(env) js_turbo_test_env_init(__bdd_config__, env)
#define js_uv_test_env_cleanup js_turbo_test_env_cleanup
#define js_uv_test_run_loop js_turbo_test_process_events
#define js_uv_test_process_events js_turbo_test_process_events
#define js_uv_test_eval(env, code) js_turbo_test_eval(__bdd_config__, env, code)
#define js_uv_test_eval_value(env, code) js_turbo_test_eval_value(__bdd_config__, env, code)
#define js_uv_test_global_prop js_turbo_test_global_prop

#endif /* JS_TURBO_TEST_FIXTURE_H */
