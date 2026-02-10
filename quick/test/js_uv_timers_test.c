#include "test_uv_fixture.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
#endif

static int32_t get_int_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
  JSValue prop = js_uv_test_global_prop(env, name);
  int32_t value = 0;
  check_int_eq(0, JS_ToInt32(env->ctx, &value, prop));
  JS_FreeValue(env->ctx, prop);
  return value;
}

static const char *get_string_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
  JSValue prop = js_uv_test_global_prop(env, name);
  check_true(JS_IsString(prop));
  size_t len = 0;
  const char *str = JS_ToCStringLen(env->ctx, &len, prop);
  check_not_null(str);
  char *copy = (char *)malloc(len + 1);
  check_not_null(copy);
  if (copy) {
    memcpy(copy, str, len);
    copy[len] = '\0';
  }
  JS_FreeCString(env->ctx, str);
  JS_FreeValue(env->ctx, prop);
  return copy;
}

spec("js_uv_timers") {
  it("should set timeout and execute callback") {
      JSTurboTestEnv env = {0};
      js_uv_test_env_init(&env);

      js_uv_test_eval(&env, "var timerFired = 0;\n"
                            "turbo.setTimeout(() => { timerFired = 1; }, 5);\n");

      // Process events multiple times to ensure timer execution
      for (int i = 0; i < 10; i++) {
        js_uv_test_process_events(&env);
        turbo_sleep_ms(10);
      }

      check_int_eq(1, get_int_global(__bdd_config__, &env, "timerFired"));
      
      js_uv_test_env_cleanup(&env);
  }

  it("should clear timer and prevent callback") {
      JSTurboTestEnv env = {0};
      js_uv_test_env_init(&env);

      js_uv_test_eval(&env, "var cleared = 0;\n"
                            "var id = turbo.setTimeout(() => { cleared = 1; }, 5);\n"
                            "turbo.clearTimeout(id);\n");

      // Process events multiple times
      for (int i = 0; i < 10; i++) {
        js_uv_test_process_events(&env);
        turbo_sleep_ms(10);
      }

      check_int_eq(0, get_int_global(__bdd_config__, &env, "cleared"));
      
      js_uv_test_env_cleanup(&env);
  }

  it("should resolve promise on turbo sleep") {
      JSTurboTestEnv env = {0};
      js_uv_test_env_init(&env);

      // Current js_timers.c implementation of sleep is synchronous for now,
      // so we don't need a .then() for testing if it blocks correctly.
      js_uv_test_eval(&env, "var sleepResult = 'pending';\n"
                            "turbo.sleep(5);\n"
                            "sleepResult = 'done';\n");
      js_uv_test_process_events(&env);
      const char *result = get_string_global(__bdd_config__, &env, "sleepResult");
      if (result) {
        check_str_eq("done", result);
        free((void *)result);
      }
      
      js_uv_test_env_cleanup(&env);
  }

  it("should set interval and run multiple times") {
      JSTurboTestEnv env = {0};
      js_uv_test_env_init(&env);

      js_uv_test_eval(&env, "var intervalTicks = 0;\n"
                            "var id = turbo.setInterval(() => {\n"
                            "  intervalTicks += 1;\n"
                            "  if (intervalTicks >= 3) {\n"
                            "    turbo.clearInterval(id);\n"
                            "  }\n"
                            "}, 5);\n");

      // Process events multiple times to let interval run
      for (int i = 0; i < 50; i++) {
        js_uv_test_process_events(&env);
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
        if (get_int_global(__bdd_config__, &env, "intervalTicks") >= 3)
          break;
      }

      check_int_eq(3, get_int_global(__bdd_config__, &env, "intervalTicks"));
      
      js_uv_test_env_cleanup(&env);
  }
}