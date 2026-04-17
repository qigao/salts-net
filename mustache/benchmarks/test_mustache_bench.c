/**
 * @file test_mustache_bench.c
 * @brief Micro-benchmarks for mustache compile/compile_v/process (tinytest)
 */

#include "tinytest.h"
#include "mustache.h"
#include "turbo_str_view.h"
#include "turbo_str.h"
#include <stdint.h>
#include <string.h>

#define BENCH_ITERS_COMPILE 20000
#define BENCH_ITERS_PROCESS 200000
#define BENCH_ITERS_COMPILE_PROCESS 20000
#define BENCH_ITERS_PROCESS_LONG 100000
#define BENCH_ITERS_PROCESS_NESTED 100000
#define BENCH_ITERS_PROCESS_ESCAPE 100000
#define BENCH_ITERS_COMPILE_LARGE 5000
#define BENCH_ITERS_PROCESS_LARGE 10000
#define BENCH_ITERS_VIEW_VS_PTR 1000000

static volatile size_t sink_size = 0;

// For view vs ptr benchmark - prevent inlining
#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

NOINLINE static size_t call_with_ptr(const char *s, size_t len) {
  return len + (s ? s[0] : 0);
}

NOINLINE static size_t call_with_view(tstr_v v) {
  return v.len + (v.data ? v.data[0] : 0);
}

NOINLINE static size_t call_with_tstr(tstr_t s) {
  return tstr_len(s) + (s ? s[0] : 0);
}

typedef struct BENCH_BUFFER {
  char data[8192];
  size_t n;
} BENCH_BUFFER;

static int out_verbatim(const char *output, size_t n, void *data) {
  BENCH_BUFFER *buf = (BENCH_BUFFER *)data;
  if (buf->n + n < sizeof(buf->data)) {
    memcpy(buf->data + buf->n, output, n);
    buf->n += n;
  }
  sink_size += n;
  return 0;
}

static int out_escaped(const char *output, size_t n, void *data) {
  BENCH_BUFFER *buf = (BENCH_BUFFER *)data;
  for (size_t i = 0; i < n; i++) {
    char c = output[i];
    const char *esc = NULL;
    size_t esc_len = 0;
    switch (c) {
    case '&':
      esc = "&amp;";
      esc_len = 5;
      break;
    case '<':
      esc = "&lt;";
      esc_len = 4;
      break;
    case '>':
      esc = "&gt;";
      esc_len = 4;
      break;
    case '"':
      esc = "&quot;";
      esc_len = 6;
      break;
    default:
      if (buf->n < sizeof(buf->data))
        buf->data[buf->n++] = c;
      sink_size++;
      continue;
    }
    if (buf->n + esc_len < sizeof(buf->data)) {
      memcpy(buf->data + buf->n, esc, esc_len);
      buf->n += esc_len;
    }
    sink_size += esc_len;
  }
  return 0;
}

static const MUSTACHE_RENDERER renderer = {
    out_verbatim,
    out_escaped,
};

static int out_verbatim_count(const char *output, size_t n, void *data) {
  (void)output;
  (void)data;
  sink_size += n;
  return 0;
}

static int out_escaped_count(const char *output, size_t n, void *data) {
  return out_verbatim_count(output, n, data);
}

static const MUSTACHE_RENDERER renderer_count = {
    out_verbatim_count,
    out_escaped_count,
};

typedef struct BENCH_PROVIDER {
  const char *name;
} BENCH_PROVIDER;

static void *get_root(void *provider_data) { return provider_data; }

static void *get_child_by_name(void *node, const char *name, size_t size, void *provider_data) {
  (void)provider_data;
  BENCH_PROVIDER *p = (BENCH_PROVIDER *)node;
  if (!p || !p->name)
    return NULL;
  if (size == 4 && memcmp(name, "name", 4) == 0)
    return (void *)p->name;
  return NULL;
}

static int dump_node(void *node, int (*out_fn)(const char *, size_t, void *),
                     void *renderer_data, void *provider_data) {
  (void)provider_data;
  const char *s = (const char *)node;
  if (!s)
    return 0;
  return out_fn(s, strlen(s), renderer_data);
}

static void *get_child_by_index(void *node, unsigned index, void *provider_data) {
  (void)node;
  (void)index;
  (void)provider_data;
  return NULL;
}

static const MUSTACHE_DATAPROVIDER provider = {
    dump_node,
    get_root,
    get_child_by_name,
    get_child_by_index,
    NULL,
    NULL,
    NULL,
};

typedef enum {
  NODE_ROOT = 0,
  NODE_STR,
  NODE_TAG_LIST
} BENCH_NODE_KIND;

typedef struct BENCH_NODE {
  BENCH_NODE_KIND kind;
  const char *str;
  void *items;
  size_t count;
} BENCH_NODE;

static const tstr_v KEY_NAME = {"name", 4};
static const tstr_v KEY_CITY = {"city", 4};
static const tstr_v KEY_TITLE = {"title", 5};
static const tstr_v KEY_USER = {"user", 4};
static const tstr_v KEY_TAGS = {"tags", 4};
static const tstr_v KEY_A = {"a", 1};
static const tstr_v KEY_B = {"b", 1};
static const tstr_v KEY_C = {"c", 1};

static void *get_root_rich(void *provider_data) { return provider_data; }

static void *get_child_by_name_rich(void *node, const char *name, size_t size,
                                    void *provider_data) {
  (void)provider_data;
  BENCH_NODE *n = (BENCH_NODE *)node;
  if (!n)
    return NULL;

  tstr_v key = tstr_v_from_buf(name, size);
  if (n->kind == NODE_ROOT) {
    if (tstr_v_eq(key, KEY_NAME))
      return (void *)((BENCH_NODE *)n + 1);
    if (tstr_v_eq(key, KEY_CITY))
      return (void *)((BENCH_NODE *)n + 2);
    if (tstr_v_eq(key, KEY_TITLE))
      return (void *)((BENCH_NODE *)n + 3);
    if (tstr_v_eq(key, KEY_USER) || tstr_v_eq(key, KEY_A) || tstr_v_eq(key, KEY_B) ||
        tstr_v_eq(key, KEY_C))
      return node;
    if (tstr_v_eq(key, KEY_TAGS))
      return (void *)((BENCH_NODE *)n + 4);
  }
  return NULL;
}

static void *get_child_by_index_rich(void *node, unsigned index, void *provider_data) {
  (void)provider_data;
  BENCH_NODE *n = (BENCH_NODE *)node;
  if (!n)
    return NULL;
  if (n->kind == NODE_ROOT) {
    if (index == 0)
      return node;
    return NULL;
  }
  if (n->kind == NODE_TAG_LIST) {
    if (index < n->count)
      return &((BENCH_NODE *)n->items)[index];
    return NULL;
  }
  return NULL;
}

static int dump_node_rich(void *node, int (*out_fn)(const char *, size_t, void *),
                          void *renderer_data, void *provider_data) {
  (void)provider_data;
  BENCH_NODE *n = (BENCH_NODE *)node;
  if (!n)
    return 0;
  if (n->kind == NODE_STR && n->str)
    return out_fn(n->str, strlen(n->str), renderer_data);
  return 0;
}

static const MUSTACHE_DATAPROVIDER provider_rich = {
    dump_node_rich,
    get_root_rich,
    get_child_by_name_rich,
    get_child_by_index_rich,
    NULL,
    NULL,
    NULL,
};

// Dashboard HTML template (~8KB) for large template benchmarks
static const char DASHBOARD_TEMPLATE[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "    <meta charset=\"UTF-8\">\n"
    "    <title>{{test_suite_name}} - Test Results Dashboard</title>\n"
    "    <style>\n"
    "        * { margin: 0; padding: 0; box-sizing: border-box; }\n"
    "        body { font-family: sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }\n"
    "        .container { max-width: 1200px; margin: 0 auto; }\n"
    "        .header { background: white; border-radius: 12px; padding: 30px; margin-bottom: 20px; }\n"
    "        .header h1 { color: #2d3748; font-size: 32px; margin-bottom: 10px; }\n"
    "        .stat-card { background: white; border-radius: 12px; padding: 25px; }\n"
    "        .stat-value { font-size: 48px; font-weight: bold; }\n"
    "        .test-item { padding: 20px; border-left: 4px solid #e2e8f0; margin-bottom: 15px; }\n"
    "        .test-item.passed { border-left-color: #48bb78; }\n"
    "        .test-item.failed { border-left-color: #f56565; background: #fff5f5; }\n"
    "    </style>\n"
    "</head>\n"
    "<body>\n"
    "    <div class=\"container\">\n"
    "        <div class=\"header\">\n"
    "            <h1>{{name}}</h1>\n"
    "            <div class=\"timestamp\">Generated: {{timestamp}}</div>\n"
    "            <div class=\"progress-bar\"><div class=\"progress-fill\" style=\"width: {{pass_percentage}}%\"></div></div>\n"
    "        </div>\n"
    "        <div class=\"stats\">\n"
    "            <div class=\"stat-card total\"><div class=\"stat-value\">{{total_tests}}</div><div class=\"stat-label\">Total</div></div>\n"
    "            <div class=\"stat-card passed\"><div class=\"stat-value\">{{passed_tests}}</div><div class=\"stat-label\">Passed</div></div>\n"
    "            <div class=\"stat-card failed\"><div class=\"stat-value\">{{failed_tests}}</div><div class=\"stat-label\">Failed</div></div>\n"
    "        </div>\n"
    "        <div class=\"tests-container\">\n"
    "            <h2>Test Cases</h2>\n"
    "            {{#testcase}}\n"
    "            <div class=\"test-item {{status}}\">\n"
    "                <div class=\"test-name\">{{name}}</div>\n"
    "                <div class=\"test-status {{status}}\">{{status}}</div>\n"
    "                <span>{{classname}}</span>\n"
    "                <span>{{time}}s</span>\n"
    "                {{#failure}}\n"
    "                <div class=\"failure-details\">\n"
    "                    <div class=\"failure-message\">{{message}}</div>\n"
    "                </div>\n"
    "                {{/failure}}\n"
    "            </div>\n"
    "            {{/testcase}}\n"
    "            {{^testcase}}\n"
    "            <div class=\"empty-state\">No test results found</div>\n"
    "            {{/testcase}}\n"
    "        </div>\n"
    "    </div>\n"
    "</body>\n"
    "</html>\n";

static const tstr_v KEY_TEST_SUITE_NAME = {"test_suite_name", 15};
static const tstr_v KEY_TIMESTAMP = {"timestamp", 9};
static const tstr_v KEY_PASS_PERCENTAGE = {"pass_percentage", 15};
static const tstr_v KEY_TOTAL_TESTS = {"total_tests", 11};
static const tstr_v KEY_PASSED_TESTS = {"passed_tests", 12};
static const tstr_v KEY_FAILED_TESTS = {"failed_tests", 12};
static const tstr_v KEY_TESTCASE = {"testcase", 8};
static const tstr_v KEY_STATUS = {"status", 6};
static const tstr_v KEY_CLASSNAME = {"classname", 9};
static const tstr_v KEY_TIME = {"time", 4};
static const tstr_v KEY_FAILURE = {"failure", 7};
static const tstr_v KEY_MESSAGE = {"message", 7};

typedef enum {
  DASH_ROOT = 0,
  DASH_STR,
  DASH_TESTCASE_LIST,
  DASH_TESTCASE,
  DASH_FAILURE
} DASH_NODE_KIND;

typedef struct DASH_NODE {
  DASH_NODE_KIND kind;
  const char *str;
  void *items;
  size_t count;
  // testcase fields
  const char *status;
  const char *classname;
  const char *time;
  const char *failure_msg;
} DASH_NODE;

static void *get_root_dash(void *provider_data) { return provider_data; }

static void *get_child_by_name_dash(void *node, const char *name, size_t size,
                                    void *provider_data) {
  (void)provider_data;
  DASH_NODE *n = (DASH_NODE *)node;
  if (!n)
    return NULL;

  tstr_v key = tstr_v_from_buf(name, size);

  if (n->kind == DASH_ROOT) {
    DASH_NODE *root = n;
    if (tstr_v_eq(key, KEY_TEST_SUITE_NAME) || tstr_v_eq(key, KEY_NAME))
      return (void *)(root + 1);
    if (tstr_v_eq(key, KEY_TIMESTAMP))
      return (void *)(root + 2);
    if (tstr_v_eq(key, KEY_PASS_PERCENTAGE))
      return (void *)(root + 3);
    if (tstr_v_eq(key, KEY_TOTAL_TESTS))
      return (void *)(root + 4);
    if (tstr_v_eq(key, KEY_PASSED_TESTS))
      return (void *)(root + 5);
    if (tstr_v_eq(key, KEY_FAILED_TESTS))
      return (void *)(root + 6);
    if (tstr_v_eq(key, KEY_TESTCASE))
      return (void *)(root + 7);
  }

  if (n->kind == DASH_TESTCASE) {
    if (tstr_v_eq(key, KEY_NAME))
      return (void *)n->str;
    if (tstr_v_eq(key, KEY_STATUS))
      return (void *)n->status;
    if (tstr_v_eq(key, KEY_CLASSNAME))
      return (void *)n->classname;
    if (tstr_v_eq(key, KEY_TIME))
      return (void *)n->time;
    if (tstr_v_eq(key, KEY_FAILURE) && n->failure_msg)
      return (void *)n;
  }

  if (n->kind == DASH_FAILURE) {
    if (tstr_v_eq(key, KEY_MESSAGE))
      return (void *)n->failure_msg;
  }

  return NULL;
}

static void *get_child_by_index_dash(void *node, unsigned index, void *provider_data) {
  (void)provider_data;
  DASH_NODE *n = (DASH_NODE *)node;
  if (!n)
    return NULL;

  if (n->kind == DASH_TESTCASE_LIST) {
    if (index < n->count)
      return &((DASH_NODE *)n->items)[index];
    return NULL;
  }

  // For failure section iteration (single item)
  if (n->kind == DASH_TESTCASE && n->failure_msg) {
    if (index == 0) {
      static DASH_NODE failure_node;
      failure_node.kind = DASH_FAILURE;
      failure_node.failure_msg = n->failure_msg;
      return &failure_node;
    }
    return NULL;
  }

  return NULL;
}

static int dump_node_dash(void *node, int (*out_fn)(const char *, size_t, void *),
                          void *renderer_data, void *provider_data) {
  (void)provider_data;
  if (!node)
    return 0;

  // Direct string pointer
  const char *s = (const char *)node;
  // Check if it's a DASH_NODE by checking if pointer is in valid range
  DASH_NODE *n = (DASH_NODE *)node;
  if (n->kind == DASH_STR && n->str)
    return out_fn(n->str, strlen(n->str), renderer_data);

  // Assume it's a raw string
  return out_fn(s, strlen(s), renderer_data);
}

static const MUSTACHE_DATAPROVIDER provider_dash = {
    dump_node_dash,
    get_root_dash,
    get_child_by_name_dash,
    get_child_by_index_dash,
    NULL,
    NULL,
    NULL,
};

spec("mustache bench") {
  const char *templ = "Hello {{name}}! {{#user}}{{name}}{{/user}}";
  const size_t templ_len = strlen(templ);
  const char *templ_escape = "Hello {{name}}! {{#user}}{{name}}{{/user}}";
  const size_t templ_escape_len = strlen(templ_escape);
  const char *templ_long =
      "Hello {{name}}! {{city}} {{#user}}"
      "{{name}} {{name}} {{name}} {{name}} {{name}} "
      "{{city}} {{city}} {{city}} {{city}} {{city}} "
      "{{#tags}}{{.}} {{/tags}}"
      "{{/user}}";
  const size_t templ_long_len = strlen(templ_long);
  const char *templ_nested =
      "{{#a}}{{#b}}{{#c}}{{name}}{{/c}}{{/b}}{{/a}}";
  const size_t templ_nested_len = strlen(templ_nested);
  const char *templ_rich =
      "Hello {{name}} from {{city}}. {{#user}}Title: {{title}}. {{/user}}"
      "Tags: {{#tags}}{{.}},{{/tags}} Done.";
  const size_t templ_rich_len = strlen(templ_rich);

  bench("compile/process") {

      benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
    benchmark("compile", BENCH_ITERS_COMPILE, 1) {
      MUSTACHE_TEMPLATE *t = mustache_compile(templ, templ_len, NULL, NULL, 0);
      if (t)
        mustache_release(t);
    }

    benchmark("compile_v", BENCH_ITERS_COMPILE, 1) {
      tstr_v v = tstr_v_from_buf(templ, templ_len);
      MUSTACHE_TEMPLATE *t = mustache_compile_v(v, NULL, NULL, 0);
      if (t)
        mustache_release(t);
    }

    BENCH_PROVIDER p = {.name = "World"};
    BENCH_BUFFER buf = {0};

    benchmark("compile+process", BENCH_ITERS_COMPILE_PROCESS, 1) {
      MUSTACHE_TEMPLATE *t = mustache_compile(templ, templ_len, NULL, NULL, 0);
      if (t) {
        buf.n = 0;
        mustache_process(t, &renderer, &buf, &provider, &p);
        sink_size += buf.n;
        mustache_release(t);
      }
    }

    MUSTACHE_TEMPLATE *compiled = mustache_compile(templ, templ_len, NULL, NULL, 0);
    check_not_null(compiled);

    benchmark("process", BENCH_ITERS_PROCESS, 1) {
      buf.n = 0;
      mustache_process(compiled, &renderer, &buf, &provider, &p);
      sink_size += buf.n;
    }

    mustache_release(compiled);

    MUSTACHE_TEMPLATE *compiled_escape =
        mustache_compile(templ_escape, templ_escape_len, NULL, NULL, 0);
    check_not_null(compiled_escape);

    // Data with HTML special chars to measure escape overhead
    BENCH_PROVIDER p_escape = {.name = "<script>alert(\"XSS\")&</script>"};

    benchmark("process(escape)", BENCH_ITERS_PROCESS_ESCAPE, 1) {
      buf.n = 0;
      mustache_process(compiled_escape, &renderer, &buf, &provider, &p_escape);
      sink_size += buf.n;
    }

    mustache_release(compiled_escape);

    MUSTACHE_TEMPLATE *compiled_long = mustache_compile(templ_long, templ_long_len, NULL, NULL, 0);
    check_not_null(compiled_long);

    benchmark("process(long)", BENCH_ITERS_PROCESS_LONG, 1) {
      buf.n = 0;
      mustache_process(compiled_long, &renderer, &buf, &provider, &p);
      sink_size += buf.n;
    }

    mustache_release(compiled_long);

    MUSTACHE_TEMPLATE *compiled_nested =
        mustache_compile(templ_nested, templ_nested_len, NULL, NULL, 0);
    check_not_null(compiled_nested);

    benchmark("process(nested)", BENCH_ITERS_PROCESS_NESTED, 1) {
      buf.n = 0;
      mustache_process(compiled_nested, &renderer, &buf, &provider, &p);
      sink_size += buf.n;
    }

    mustache_release(compiled_nested);

    static BENCH_NODE tag_nodes[] = {
        {NODE_STR, "alpha", NULL, 0},
        {NODE_STR, "beta", NULL, 0},
        {NODE_STR, "gamma", NULL, 0},
        {NODE_STR, "delta", NULL, 0},
    };
    BENCH_NODE nodes[] = {
        {NODE_ROOT, NULL, NULL, 0},
        {NODE_STR, "World", NULL, 0},
        {NODE_STR, "Gotham", NULL, 0},
        {NODE_STR, "Engineer", NULL, 0},
        {NODE_TAG_LIST, NULL, tag_nodes, 4},
    };

    MUSTACHE_TEMPLATE *compiled_rich =
        mustache_compile(templ_rich, templ_rich_len, NULL, NULL, 0);
    check_not_null(compiled_rich);

    benchmark("process(rich)", BENCH_ITERS_PROCESS_LONG, 1) {
      buf.n = 0;
      mustache_process(compiled_rich, &renderer, &buf, &provider_rich, nodes);
      sink_size += buf.n;
    }

    benchmark("process(rich,count)", BENCH_ITERS_PROCESS_LONG, 1) {
      mustache_process(compiled_rich, &renderer_count, NULL, &provider_rich, nodes);
    }

    mustache_release(compiled_rich);

    // Large template benchmarks (dashboard.html ~2KB condensed)
    const size_t dashboard_len = sizeof(DASHBOARD_TEMPLATE) - 1;

    benchmark("compile(large)", BENCH_ITERS_COMPILE_LARGE, 1) {
      MUSTACHE_TEMPLATE *t = mustache_compile(DASHBOARD_TEMPLATE, dashboard_len, NULL, NULL, 0);
      if (t)
        mustache_release(t);
    }

    // Dashboard data: root + 6 string nodes + testcase list
    static DASH_NODE testcases[] = {
        {DASH_TESTCASE, "test_basic_parse", NULL, 0, "passed", "json_parser", "0.001", NULL},
        {DASH_TESTCASE, "test_nested_objects", NULL, 0, "passed", "json_parser", "0.002", NULL},
        {DASH_TESTCASE, "test_array_handling", NULL, 0, "failed", "json_parser", "0.015",
         "Expected array length 5, got 4"},
        {DASH_TESTCASE, "test_unicode_escape", NULL, 0, "passed", "json_parser", "0.003", NULL},
        {DASH_TESTCASE, "test_large_file", NULL, 0, "passed", "json_parser", "0.125", NULL},
    };
    DASH_NODE dash_nodes[] = {
        {DASH_ROOT, NULL, NULL, 0, NULL, NULL, NULL, NULL},
        {DASH_STR, "JSON Parser Tests", NULL, 0, NULL, NULL, NULL, NULL},
        {DASH_STR, "2024-01-15 14:30:00", NULL, 0, NULL, NULL, NULL, NULL},
        {DASH_STR, "80", NULL, 0, NULL, NULL, NULL, NULL},
        {DASH_STR, "5", NULL, 0, NULL, NULL, NULL, NULL},
        {DASH_STR, "4", NULL, 0, NULL, NULL, NULL, NULL},
        {DASH_STR, "1", NULL, 0, NULL, NULL, NULL, NULL},
        {DASH_TESTCASE_LIST, NULL, testcases, 5, NULL, NULL, NULL, NULL},
    };

    MUSTACHE_TEMPLATE *compiled_dash =
        mustache_compile(DASHBOARD_TEMPLATE, dashboard_len, NULL, NULL, 0);
    check_not_null(compiled_dash);

    benchmark("process(large)", BENCH_ITERS_PROCESS_LARGE, 1) {
      buf.n = 0;
      mustache_process(compiled_dash, &renderer, &buf, &provider_dash, dash_nodes);
      sink_size += buf.n;
    }

    benchmark("compile+process(large)", BENCH_ITERS_COMPILE_LARGE, 1) {
      MUSTACHE_TEMPLATE *t = mustache_compile(DASHBOARD_TEMPLATE, dashboard_len, NULL, NULL, 0);
      if (t) {
        buf.n = 0;
        mustache_process(t, &renderer, &buf, &provider_dash, dash_nodes);
        sink_size += buf.n;
        mustache_release(t);
      }
    }

    mustache_release(compiled_dash);
  }

  bench("view vs ptr") {

      benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
    const char *test_str = "hello world test string";
    size_t test_len = 23;
    tstr_v test_view = tstr_v_from_buf(test_str, test_len);
    tstr_t test_tstr = tstr_from_v(test_view);

    benchmark("call(ptr,len)", BENCH_ITERS_VIEW_VS_PTR, 1) {
      sink_size += call_with_ptr(test_str, test_len);
    }

    benchmark("call(view)", BENCH_ITERS_VIEW_VS_PTR, 1) {
      sink_size += call_with_view(test_view);
    }

    benchmark("call(tstr)", BENCH_ITERS_VIEW_VS_PTR, 1) {
      sink_size += call_with_tstr(test_tstr);
    }

    tstr_free(test_tstr);
  }
}
