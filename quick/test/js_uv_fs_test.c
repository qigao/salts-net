#include "test_uv_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <direct.h>
  #include <windows.h>
#else
  #include <sys/stat.h>
  #include <unistd.h>
#endif

static void make_temp_path(__bdd_config_type__ *__bdd_config__, char *out, size_t out_len) {
  char tmpdir[512];
#ifdef _WIN32
  DWORD len = GetTempPathA(sizeof(tmpdir), tmpdir);
  check_true(len > 0 && len < sizeof(tmpdir));
#else
  const char *tmp = getenv("TMPDIR");
  if (!tmp || strlen(tmp) == 0) tmp = "/tmp";
  strncpy(tmpdir, tmp, sizeof(tmpdir) - 1);
  tmpdir[sizeof(tmpdir) - 1] = '\0';
  size_t len = strlen(tmpdir);
#endif
  
  int need_sep = (len == 0 || (tmpdir[len - 1] != '/' && tmpdir[len - 1] != '\\'));
  unsigned long long unique = (unsigned long long)time(NULL) * 1000 + rand() % 1000;
  if (need_sep) {
#ifdef _WIN32
    snprintf(out, out_len, "%s\\turbotest_%llu.txt", tmpdir, unique);
#else
    snprintf(out, out_len, "%s/turbotest_%llu.txt", tmpdir, unique);
#endif
  } else {
    snprintf(out, out_len, "%sturbotest_%llu.txt", tmpdir, unique);
  }
}

static void make_temp_dir(__bdd_config_type__ *__bdd_config__, char *out, size_t out_len) {
  char tmpdir[512];
#ifdef _WIN32
  DWORD len = GetTempPathA(sizeof(tmpdir), tmpdir);
  check_true(len > 0 && len < sizeof(tmpdir));
#else
  const char *tmp = getenv("TMPDIR");
  if (!tmp || strlen(tmp) == 0) tmp = "/tmp";
  strncpy(tmpdir, tmp, sizeof(tmpdir) - 1);
  tmpdir[sizeof(tmpdir) - 1] = '\0';
  size_t len = strlen(tmpdir);
#endif
  
  int need_sep = (len == 0 || (tmpdir[len - 1] != '/' && tmpdir[len - 1] != '\\'));
#ifdef _WIN32
  const char sep = '\\';
#else
  const char sep = '/';
#endif
  unsigned long long unique = (unsigned long long)time(NULL) * 1000 + rand() % 1000;
  int written;
  if (need_sep) {
    written = snprintf(out, out_len, "%s%cturbotest_dir_%llu", tmpdir, sep, unique);
  } else {
    written = snprintf(out, out_len, "%sturbotest_dir_%llu", tmpdir, unique);
  }
  check_true(written > 0);
  check_true((size_t)written < out_len);
#ifdef _WIN32
  check_int_eq(0, _mkdir(out));
#else
  check_int_eq(0, mkdir(out, 0700));
#endif
}

static void escape_js_string(const char *input, char *output, size_t out_len) {
  size_t j = 0;
  for (size_t i = 0; input[i] != '\0' && j + 2 < out_len; ++i) {
    char c = input[i];
    if (c == '\\' || c == '"') {
      output[j++] = '\\';
    }
    output[j++] = c;
  }
  output[j] = '\0';
}

static const char *dup_string_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
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

static JSValue get_global(__bdd_config_type__ *__bdd_config__, JSTurboTestEnv* env, const char *name) {
  JSValue prop = js_uv_test_global_prop(env, name);
  check_false(JS_IsException(prop));
  return prop;
}

spec("js_uv_fs") {
    it("should write and read file") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        char path[512];
        make_temp_path(__bdd_config__, path, sizeof(path));
        char literal[1024];
        escape_js_string(path, literal, sizeof(literal));

        char script[2048];
        snprintf(script, sizeof(script),
                "const fs = turbo.fs;\n"
                "var fsResult = null;\n"
                "try {\n"
                "  fs.writeFile(\"%s\", \"unity_file_data\");\n"
                "  const data = fs.readFile(\"%s\");\n"
                "  var fsResult = data;\n"
                "} catch (err) {\n"
                "  var fsResult = 'ERROR:' + err.message;\n"
                "}\n",
                literal, literal);
        js_uv_test_eval(&env, script);

        const char *result = dup_string_global(__bdd_config__, &env, "fsResult");
        check_str_eq("unity_file_data", result);
        if (result) free((void *)result);

        remove(path);
        
        js_uv_test_env_cleanup(&env);
    }

    it("should stat file") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        char path[512];
        make_temp_path(__bdd_config__, path, sizeof(path));
        FILE *fp = fopen(path, "wb");
        check_not_null(fp);
        if (fp) {
            fputs("stat-data", fp);
            fclose(fp);
        }

        char literal[1024];
        escape_js_string(path, literal, sizeof(literal));

        char script[1024];
        snprintf(script, sizeof(script),
                "const fs = turbo.fs;\n"
                "var statResult = null;\n"
                "try {\n"
                "  const info = fs.stat(\"%s\");\n"
                "  var statResult = info ? info.isFile : false;\n"
                "} catch (err) {\n"
                "  var statResult = false;\n"
                "}\n",
                literal);
        js_uv_test_eval(&env, script);

        JSValue result = get_global(__bdd_config__, &env, "statResult");
        check_true(JS_IsBool(result));
        check_true(JS_ToBool(env.ctx, result));
        JS_FreeValue(env.ctx, result);

        remove(path);

        js_uv_test_env_cleanup(&env);
    }

    it("should readdir") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        char dir[512];
        make_temp_dir(__bdd_config__, dir, sizeof(dir));
    #ifdef _WIN32
        const char sep = '\\';
    #else
        const char sep = '/';
    #endif
        const char *filename = "turbo_entry.txt";
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%c%s", dir, sep, filename);
        FILE *fp = fopen(file_path, "wb");
        check_not_null(fp);
        if (fp) {
            fputs("dir-data", fp);
            fclose(fp);
        }

        char literal[1024];
        escape_js_string(dir, literal, sizeof(literal));

        char script[2048];
        snprintf(script, sizeof(script),
                "const fs = turbo.fs;\n"
                "var dirResult = null;\n"
                "try {\n"
                "  const entries = fs.readdir(\"%s\");\n"
                "  var dirResult = entries.sort().join(\",\");\n"
                "} catch (err) {\n"
                "  var dirResult = 'ERROR:' + err.message;\n"
                "}\n",
                literal);
        js_uv_test_eval(&env, script);

        const char *result = dup_string_global(__bdd_config__, &env, "dirResult");
        check_str_eq(filename, result);
        if (result) free((void *)result);

        check_int_eq(0, remove(file_path));
    #ifdef _WIN32
        check_int_eq(0, _rmdir(dir));
    #else
        check_int_eq(0, rmdir(dir));
    #endif
        
        js_uv_test_env_cleanup(&env);
    }
}
