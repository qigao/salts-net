#include "unity.h"

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

static JSTurboTestEnv env;

void setUp(void) { js_turbo_test_env_init(&env); }

void tearDown(void) { js_turbo_test_env_cleanup(&env); }

static void make_temp_path(char *out, size_t out_len) {
  char tmpdir[512];
#ifdef _WIN32
  DWORD len = GetTempPathA(sizeof(tmpdir), tmpdir);
  TEST_ASSERT_TRUE(len > 0 && len < sizeof(tmpdir));
#else
  const char *tmp = getenv("TMPDIR");
  if (!tmp) tmp = "/tmp";
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

static void make_temp_dir(char *out, size_t out_len) {
  char tmpdir[512];
#ifdef _WIN32
  DWORD len = GetTempPathA(sizeof(tmpdir), tmpdir);
  TEST_ASSERT_TRUE(len > 0 && len < sizeof(tmpdir));
#else
  const char *tmp = getenv("TMPDIR");
  if (!tmp) tmp = "/tmp";
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
  TEST_ASSERT_TRUE(written > 0);
  TEST_ASSERT_TRUE((size_t)written < out_len);
#ifdef _WIN32
  TEST_ASSERT_EQUAL_INT(0, _mkdir(out));
#else
  TEST_ASSERT_EQUAL_INT(0, mkdir(out, 0700));
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

static const char *dup_string_global(const char *name) {
  JSValue prop = js_turbo_test_global_prop(&env, name);
  TEST_ASSERT_TRUE(JS_IsString(prop));
  size_t len = 0;
  const char *str = JS_ToCStringLen(env.ctx, &len, prop);
  TEST_ASSERT_NOT_NULL(str);
  char *copy = (char *)malloc(len + 1);
  TEST_ASSERT_NOT_NULL(copy);
  memcpy(copy, str, len);
  copy[len] = '\0';
  JS_FreeCString(env.ctx, str);
  JS_FreeValue(env.ctx, prop);
  return copy;
}

static JSValue get_global(const char *name) {
  JSValue prop = js_turbo_test_global_prop(&env, name);
  TEST_ASSERT_FALSE(JS_IsException(prop));
  return prop;
}

void test_fs_write_and_read_file(void) {
  char path[512];
  make_temp_path(path, sizeof(path));
  char literal[1024];
  escape_js_string(path, literal, sizeof(literal));

  char script[2048];
  snprintf(script, sizeof(script),
           "const fs = turbo.fs;\n"
           "globalThis.fsResult = null;\n"
           "try {\n"
           "  fs.writeFile(\"%s\", \"unity_file_data\");\n"
           "  const data = fs.readFile(\"%s\");\n"
           "  globalThis.fsResult = data;\n"
           "} catch (err) {\n"
           "  globalThis.fsResult = 'ERROR:' + err.message;\n"
           "}\n",
           literal, literal);
  js_turbo_test_eval(&env, script);

  const char *result = dup_string_global("fsResult");
  TEST_ASSERT_EQUAL_STRING("unity_file_data", result);
  free((void *)result);

  remove(path);
}

void test_fs_stat_reports_file(void) {
  char path[512];
  make_temp_path(path, sizeof(path));
  FILE *fp = fopen(path, "wb");
  TEST_ASSERT_NOT_NULL(fp);
  fputs("stat-data", fp);
  fclose(fp);

  char literal[1024];
  escape_js_string(path, literal, sizeof(literal));

  char script[1024];
  snprintf(script, sizeof(script),
           "const fs = turbo.fs;\n"
           "globalThis.statResult = null;\n"
           "try {\n"
           "  const info = fs.stat(\"%s\");\n"
           "  globalThis.statResult = info ? info.isFile : false;\n"
           "} catch (err) {\n"
           "  globalThis.statResult = false;\n"
           "}\n",
           literal);
  js_turbo_test_eval(&env, script);

  JSValue result = get_global("statResult");
  TEST_ASSERT_TRUE(JS_IsBool(result));
  TEST_ASSERT_TRUE(JS_ToBool(env.ctx, result));
  JS_FreeValue(env.ctx, result);

  remove(path);
}

void test_fs_readdir_lists_file(void) {
  char dir[512];
  make_temp_dir(dir, sizeof(dir));
#ifdef _WIN32
  const char sep = '\\';
#else
  const char sep = '/';
#endif
  const char *filename = "turbo_entry.txt";
  char file_path[512];
  snprintf(file_path, sizeof(file_path), "%s%c%s", dir, sep, filename);
  FILE *fp = fopen(file_path, "wb");
  TEST_ASSERT_NOT_NULL(fp);
  fputs("dir-data", fp);
  fclose(fp);

  char literal[1024];
  escape_js_string(dir, literal, sizeof(literal));

  char script[2048];
  snprintf(script, sizeof(script),
           "const fs = turbo.fs;\n"
           "globalThis.dirResult = null;\n"
           "try {\n"
           "  const entries = fs.readdir(\"%s\");\n"
           "  globalThis.dirResult = entries.sort().join(\",\");\n"
           "} catch (err) {\n"
           "  globalThis.dirResult = 'ERROR:' + err.message;\n"
           "}\n",
           literal);
  js_turbo_test_eval(&env, script);

  const char *result = dup_string_global("dirResult");
  TEST_ASSERT_EQUAL_STRING(filename, result);
  free((void *)result);

  TEST_ASSERT_EQUAL_INT(0, remove(file_path));
#ifdef _WIN32
  TEST_ASSERT_EQUAL_INT(0, _rmdir(dir));
#else
  TEST_ASSERT_EQUAL_INT(0, rmdir(dir));
#endif
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_fs_write_and_read_file);
  RUN_TEST(test_fs_stat_reports_file);
  RUN_TEST(test_fs_readdir_lists_file);
  return UNITY_END();
}
