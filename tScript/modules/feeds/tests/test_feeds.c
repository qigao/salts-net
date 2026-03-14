/**
 * @file test_feeds.c
 * @brief Integration tests for feeds_plugin (csv/json/xml via turbo_parser).
 */
#include "exprtk.h"
#include "tinytest.h"
#include "ts_plugin_loader.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define FEEDS_PLUGIN_DLL "feeds_plugin.dll"
#else
#define FEEDS_PLUGIN_DLL "libfeeds_plugin.so"
#endif

typedef struct {
  ts_plugin_handle_t *feeds_plugin;
  exprtk_env_t env;
  mem_pool_t scratch;
} test_env_t;

static void test_env_init(test_env_t *t) {
  memset(t, 0, sizeof(*t));
  exprtk_env_init(&t->env);
  mem_init(&t->scratch, 4096);
  t->feeds_plugin = ts_plugin_load(FEEDS_PLUGIN_DLL);
  if (t->feeds_plugin) {
    ts_plugin_init(t->feeds_plugin, &t->env, &t->scratch);
  }
}

static void test_env_free(test_env_t *t) {
  ts_plugin_unload(t->feeds_plugin);
  exprtk_env_free(&t->env);
  mem_destroy(&t->scratch);
}

static exprtk_value_t call_fn(test_env_t *t, const char *name, size_t argc, exprtk_value_t *args) {
  exprtk_func_t *fn = t->env.funcs;
  while (fn) {
    if (!fn->is_script && strcmp(fn->name, name) == 0) {
      return fn->data.native.fn(argc, args, fn->data.native.user_data);
    }
    fn = fn->next;
  }
  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = -999.0};
}

static exprtk_value_t make_str(test_env_t *t, const char *s) {
  size_t len = strlen(s);
  char *buf = (char *)mem_alloc(&t->env.arena, len + 1);
  memcpy(buf, s, len + 1);
  return (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = tstr_v_from_buf(buf, len)};
}

static exprtk_value_t make_num(double n) {
  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = n};
}

spec("feeds_plugin") {
  describe("plugin") {
    it("should load and unload feeds plugin via DLL") {
      ts_plugin_handle_t *h = ts_plugin_load(FEEDS_PLUGIN_DLL);
      check_not_null(h);
      check_not_null(h->plugin);
      check_str_eq(h->plugin->name, "feeds");

      exprtk_env_t env;
      mem_pool_t scratch;
      exprtk_env_init(&env);
      mem_init(&scratch, 4096);
      check_int_eq(ts_plugin_init(h, &env, &scratch), 0);

      ts_plugin_unload(h);
      exprtk_env_free(&env);
      mem_destroy(&scratch);
    }
  }

  describe("csv/json/xml") {
    it("should run csv functions via feeds") {
      test_env_t t;
      test_env_init(&t);
      check_not_null(t.feeds_plugin);

      exprtk_value_t csv_arg[1] = {make_str(&t, "a,b\n1,2\n3,4\n")};
      exprtk_value_t rows = call_fn(&t, "csv.rows", 1, csv_arg);
      exprtk_value_t cols = call_fn(&t, "csv.cols", 1, csv_arg);
      check_int_eq(rows.type, EXPRTK_VAL_NUMBER);
      check_int_eq(cols.type, EXPRTK_VAL_NUMBER);
      check_float_eq(rows.data.number, 2.0, 0.01);
      check_float_eq(cols.data.number, 2.0, 0.01);

      exprtk_value_t get_num_args[3] = {make_str(&t, "a,b\n1,2\n3,4\n"), make_num(1), make_num(1)};
      exprtk_value_t v = call_fn(&t, "csv.get_num", 3, get_num_args);
      check_int_eq(v.type, EXPRTK_VAL_NUMBER);
      check_float_eq(v.data.number, 4.0, 0.01);

      exprtk_value_t filter_args[2] = {
          make_str(&t, "price_n,volume_n\n50,100\n150,200\n80,300\n200,400\n"),
          make_str(&t, "price > 100")};
      exprtk_value_t fc = call_fn(&t, "csv.filter_count", 2, filter_args);
      check_int_eq(fc.type, EXPRTK_VAL_NUMBER);
      check_float_eq(fc.data.number, 2.0, 0.01);

      test_env_free(&t);
    }

    it("should run json.query via feeds") {
      test_env_t t;
      test_env_init(&t);
      check_not_null(t.feeds_plugin);

      exprtk_value_t args[2] = {make_str(&t, "{\"price\": 42.5}"), make_str(&t, "price")};
      exprtk_value_t res = call_fn(&t, "json.query", 2, args);
      check_int_eq(res.type, EXPRTK_VAL_NUMBER);
      check_float_eq(res.data.number, 42.5, 0.01);

      test_env_free(&t);
    }

    it("should run xml.root_name via feeds") {
      test_env_t t;
      test_env_init(&t);
      check_not_null(t.feeds_plugin);

      exprtk_value_t args[1] = {make_str(&t, "<root><x>1</x></root>")};
      exprtk_value_t res = call_fn(&t, "xml.root_name", 1, args);
      check_int_eq(res.type, EXPRTK_VAL_STRING);
      check(res.data.string.len == 4);
      check(memcmp(res.data.string.data, "root", 4) == 0);

      test_env_free(&t);
    }
  }
}
