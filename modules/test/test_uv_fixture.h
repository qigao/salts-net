#ifndef JS_TURBO_TEST_FIXTURE_H
#define JS_TURBO_TEST_FIXTURE_H
#include <string.h>

#include "js_internal.h"
#include "quickjs.h"
#include "unity.h"

typedef struct
{
  JSRuntime* rt;
  JSContext* ctx;
} JSTurboTestEnv;

static inline void js_turbo_test_env_init(JSTurboTestEnv* env)
{
  memset(env, 0, sizeof(*env));
  env->rt = JS_NewRuntime();
  TEST_ASSERT_NOT_NULL(env->rt);
  JS_SetMemoryLimit(env->rt, -1);
  JS_SetMaxStackSize(env->rt, 0);
  env->ctx = JS_NewContext(env->rt);
  TEST_ASSERT_NOT_NULL(env->ctx);
  TEST_ASSERT_EQUAL_INT(0, js_init_turbo_module(env->ctx));
}

static inline void js_turbo_test_env_cleanup(JSTurboTestEnv* env)
{
  if (env->ctx) {
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

static inline void js_turbo_test_eval(JSTurboTestEnv* env, const char* code)
{
  JSValue result =
      JS_Eval(env->ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_GLOBAL);
  TEST_ASSERT_FALSE(JS_IsException(result));
  JS_FreeValue(env->ctx, result);
}

static inline JSValue js_turbo_test_eval_value(JSTurboTestEnv* env, const char* code)
{
  JSValue result =
      JS_Eval(env->ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_GLOBAL);
  TEST_ASSERT_FALSE(JS_IsException(result));
  return result;
}

static inline JSValue js_turbo_test_global_prop(JSTurboTestEnv* env, const char* name)
{
  JSValue global_obj = JS_GetGlobalObject(env->ctx);
  JSValue prop = JS_GetPropertyStr(env->ctx, global_obj, name);
  JS_FreeValue(env->ctx, global_obj);
  return prop;
}

// Legacy compatibility macros
#define JSUVTestEnv JSTurboTestEnv
#define js_uv_test_env_init js_turbo_test_env_init
#define js_uv_test_env_cleanup js_turbo_test_env_cleanup
#define js_uv_test_run_loop js_turbo_test_process_events
#define js_uv_test_eval js_turbo_test_eval
#define js_uv_test_eval_value js_turbo_test_eval_value
#define js_uv_test_global_prop js_turbo_test_global_prop

#endif /* JS_TURBO_TEST_FIXTURE_H */
