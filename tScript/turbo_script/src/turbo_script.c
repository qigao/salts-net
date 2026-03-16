#include "turbo_script.h"
#include "exprtk.h"
#include "ts_plugin.h"
#include "ts_plugin_loader.h"
#include "turbo_buffer.h"
#include "turbo_fs.h"
#include "turbo_script_internal.h"
#include <mir-gen.h>
#include <mir.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void turbo_script_free(turbo_script_ctx_t *ctx) {
  if (!ctx)
    return;
  if (ctx->expr)
    exprtk_free(ctx->expr);

  // Free imported modules
  imported_module_t *mod = ctx->imports;
  while (mod) {
    imported_module_t *next = mod->next;
    free(mod->name);
    if (mod->expr)
      exprtk_free(mod->expr);
    free(mod);
    mod = next;
  }

  /* Phase 15: finish gen context if it was initialized */
  if (ctx->mir_ctx) {
    if (ctx->mir_gen_initialized)
      MIR_gen_finish(ctx->mir_ctx);
    MIR_finish(ctx->mir_ctx);
  }

  exprtk_env_free(&ctx->env);

  // Unload plugins after env teardown, so module/function pointers are no longer referenced.
  for (size_t i = 0; i < ctx->plugin_count; ++i) {
    ts_plugin_unload(ctx->plugins[i]);
    free(ctx->loaded_names[i]);
  }

  mem_destroy(&ctx->scratch_arena);
  free(ctx);
}

static void set_error_msg(turbo_script_ctx_t *ctx, const char *msg) {
  if (ctx) {
    if (msg)
      strncpy(ctx->error_msg, msg, sizeof(ctx->error_msg) - 1);
    else
      ctx->error_msg[0] = '\0';
  }
}

/* Zero-value shorthand */
#define TS_ZERO ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0})
#define TS_ERROR(ctx, msg)                                                                         \
  do {                                                                                             \
    set_error_msg(ctx, msg);                                                                       \
    (ctx)->env.aborted = 1;                                                                        \
  } while (0)

/* Allocate a null-terminated C string copy in the arena (no free needed) */
static inline char *arena_cstr(mem_pool_t *a, tstr_v sv) {
  char *buf = mem_alloc(a, sv.len + 1);
  if (buf) {
    memcpy(buf, sv.data, sv.len);
    buf[sv.len] = '\0';
  }
  return buf;
}

/* ── Plugin helpers ───────────────────────────────────────────────── */

/* Check if a plugin with the given import name is already loaded */
static int ts_plugin_already_loaded(turbo_script_ctx_t *ctx, const char *name) {
  for (size_t i = 0; i < ctx->plugin_count; ++i) {
    if (ctx->loaded_names[i] && strcmp(ctx->loaded_names[i], name) == 0)
      return 1;
  }
  return 0;
}

/* Build platform-specific DLL filename from plugin name */
static const char *ts_plugin_dll_name(mem_pool_t *a, const char *name) {
#ifdef _WIN32
  size_t len = strlen(name);
  char *buf = mem_alloc(a, len + 12); /* name + "_plugin.dll\0" */
  if (buf)
    sprintf(buf, "%s_plugin.dll", name);
  return buf;
#else
  size_t len = strlen(name);
  char *buf = mem_alloc(a, len + 15); /* "lib" + name + "_plugin.so\0" */
  if (buf)
    sprintf(buf, "lib%s_plugin.so", name);
  return buf;
#endif
}

/* Load a plugin by logical name ("io", "fin", ...) */
static int ts_load_plugin(turbo_script_ctx_t *ctx, const char *name) {
  if (ts_plugin_already_loaded(ctx, name))
    return 0;
  if (ctx->plugin_count >= TS_MAX_PLUGINS)
    return -1;

  const char *dll = ts_plugin_dll_name(&ctx->scratch_arena, name);
  if (!dll)
    return -1;

  ts_plugin_handle_t *h = ts_plugin_load(dll);
  if (!h)
    return -1;

  if (ts_plugin_init(h, &ctx->env, &ctx->scratch_arena) != 0) {
    ts_plugin_unload(h);
    return -1;
  }

  size_t idx = ctx->plugin_count++;
  ctx->plugins[idx] = h;
  ctx->loaded_names[idx] = strdup(name);
  return 0;
}

static int ts_is_script_import(const char *name) {
  if (!name || !*name)
    return 0;
  if (strstr(name, ".ts") != NULL)
    return 1;
  if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL)
    return 1;
  return 0;
}

static int ts_script_already_imported(const turbo_script_ctx_t *ctx, const char *name) {
  const imported_module_t *mod = ctx ? ctx->imports : NULL;
  while (mod) {
    if (mod->name && strcmp(mod->name, name) == 0)
      return 1;
    mod = mod->next;
  }
  return 0;
}

static int ts_mark_script_imported(turbo_script_ctx_t *ctx, const char *name) {
  imported_module_t *mod = (imported_module_t *)calloc(1, sizeof(*mod));
  if (!mod)
    return -1;
  mod->name = strdup(name);
  if (!mod->name) {
    free(mod);
    return -1;
  }
  mod->next = ctx->imports;
  ctx->imports = mod;
  return 0;
}

static int ts_run_script_file_in_env(turbo_script_ctx_t *ctx, const char *filename) {
  turbo_fs_buf_t buf;
  exprtk_node_t *ast = NULL;
  int rc = -1;

  if (turbo_fs_read_file(filename, &buf) != 0)
    return -1;

  char *script = (char *)malloc(buf.len + 1);
  if (!script) {
    turbo_fs_buf_free(&buf);
    return -1;
  }

  memcpy(script, buf.base, buf.len);
  script[buf.len] = '\0';
  turbo_fs_buf_free(&buf);

  ast = exprtk_parse(script, 0);
  if (!ast) {
    set_error_msg(ctx, "import: parse error");
    goto done;
  }

  if (exprtk_validate(ast, &ctx->env, ctx->error_msg, sizeof(ctx->error_msg)) != 0) {
    goto done;
  }

  (void)exprtk_eval(ast, &ctx->env);
  rc = ctx->env.aborted ? -1 : 0;

done:
  if (ast)
    exprtk_free(ast);
  free(script);
  return rc;
}

static exprtk_value_t ts_import(size_t argc, exprtk_value_t *args, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
    TS_ERROR(ctx, "import: expected 1 string arg");
    return TS_ZERO;
  }

  char *name = arena_cstr(&ctx->scratch_arena, args[0].data.string);
  if (!name) {
    TS_ERROR(ctx, "import: out of memory");
    return TS_ZERO;
  }

  if (ts_is_script_import(name)) {
    if (!ts_script_already_imported(ctx, name)) {
      if (ts_run_script_file_in_env(ctx, name) != 0) {
        TS_ERROR(ctx, "import: failed to load script");
        return TS_ZERO;
      }
      if (ts_mark_script_imported(ctx, name) != 0) {
        TS_ERROR(ctx, "import: out of memory");
        return TS_ZERO;
      }
    }
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 1.0};
  }

  if (ts_load_plugin(ctx, name) == 0) {
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 1.0};
  }

  TS_ERROR(ctx, "import: failed to load plugin");
  return TS_ZERO;
}

static exprtk_value_t ts_print(size_t argc, exprtk_value_t *args, void *user_data) {
  (void)user_data;
  for (size_t i = 0; i < argc; ++i) {
    if (i > 0)
      printf(" ");
    switch (args[i].type) {
    case EXPRTK_VAL_NUMBER:
      printf("%g", args[i].data.number);
      break;
    case EXPRTK_VAL_STRING:
      printf("%.*s", (int)args[i].data.string.len, args[i].data.string.data);
      break;
    case EXPRTK_VAL_VECTOR: {
      size_t n = args[i].data.vector.size;
      printf("[");
      for (size_t j = 0; j < n; ++j) {
        if (j > 0)
          printf(", ");
        printf("%g", args[i].data.vector.data[j]);
      }
      printf("]");
      break;
    }
    case EXPRTK_VAL_MAP:
      printf("{map:%zu}", exprtk_map_count(&args[i]));
      break;
    case EXPRTK_VAL_LIST:
      printf("[list:%zu]", args[i].data.list.count);
      break;
    case EXPRTK_VAL_NULL:
      printf("null");
      break;
    default:
      printf("[unknown type %d]", args[i].type);
      break;
    }
  }
  printf("\n");
  fflush(stdout);
  return TS_ZERO;
}

#include "turbo_script_internal.h"

turbo_script_ctx_t *turbo_script_init(turbo_script_init_flags_t flags) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)calloc(1, sizeof(turbo_script_ctx_t));
  if (!ctx)
    return NULL;

  exprtk_env_init(&ctx->env);
  mem_init(&ctx->scratch_arena, 4096);

  exprtk_env_register_func(&ctx->env, "import", ts_import, ctx);

  if (flags == TURBO_SCRIPT_INIT_DEFAULT) {
    exprtk_env_register_func(&ctx->env, "print", ts_print, ctx);
  }

  turbo_script_register_modules();
  turbo_script_register_mir(ctx);

  return ctx;
}

int turbo_script_load_plugin(turbo_script_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return -1;
  return ts_load_plugin(ctx, name);
}

int turbo_script_run(turbo_script_ctx_t *ctx, const char *script) {
  if (!ctx || !script)
    return -1;

  /* Always use interpreter for turbo_script_run() - stable and feature-complete */
  mem_reset(&ctx->scratch_arena);
  ctx->env.aborted = 0;
  ctx->env.curr_nodes = 0;
  ctx->env.curr_loop_iterations = 0;
  ctx->env.curr_recursion = 0;

  if (ctx->expr) {
    exprtk_free(ctx->expr);
    ctx->expr = NULL;
  }

  ctx->expr = exprtk_parse(script, 0);
  if (!ctx->expr) {
    set_error_msg(ctx, "Parse error");
    return -1;
  }

  if (exprtk_validate(ctx->expr, &ctx->env, ctx->error_msg, sizeof(ctx->error_msg)) != 0) {
    return -1;
  }

  exprtk_value_t res = exprtk_eval(ctx->expr, &ctx->env);
  (void)res;

  return ctx->env.aborted ? -1 : 0;
}

static void print_repl_val(exprtk_value_t *val, turbo_script_ctx_t *ctx) {
  switch (val->type) {
    case EXPRTK_VAL_NUMBER:
      printf("%g\n", val->data.number);
      break;
    case EXPRTK_VAL_STRING:
      printf("%.*s\n", (int)val->data.string.len, val->data.string.data);
      break;
    case EXPRTK_VAL_VECTOR: {
      size_t n = val->data.vector.size;
      printf("[");
      for (size_t j = 0; j < n; ++j) {
        if (j > 0) printf(", ");
        printf("%g", val->data.vector.data[j]);
      }
      printf("]\n");
      break;
    }
    case EXPRTK_VAL_MAP:
      printf("{map}\n");
      break;
    case EXPRTK_VAL_LIST:
      printf("[list]\n");
      break;
    case EXPRTK_VAL_NULL:
      printf("null\n");
      break;
    case EXPRTK_VAL_FUNCTION:
      printf("[function]\n");
      break;
    default:
      printf("[unknown]\n");
      break;
  }
}

int turbo_script_repl_run(turbo_script_ctx_t *ctx, const char *script) {
  if (!ctx || !script)
    return -1;

  /* Reset scratch arena from previous run */
  mem_reset(&ctx->scratch_arena);
  ctx->env.aborted = 0;
  ctx->env.curr_nodes = 0;
  ctx->env.curr_loop_iterations = 0;
  ctx->env.curr_recursion = 0;

  if (ctx->expr) {
    exprtk_free(ctx->expr);
    ctx->expr = NULL;
  }

  ctx->expr = exprtk_parse(script, 0);
  if (!ctx->expr) {
    set_error_msg(ctx, "Parse error");
    return -1;
  }

  if (exprtk_validate(ctx->expr, &ctx->env, ctx->error_msg, sizeof(ctx->error_msg)) != 0) {
    return -1;
  }

  exprtk_value_t res = exprtk_eval(ctx->expr, &ctx->env);
  if (!ctx->env.aborted && ctx->expr) {
     int is_block = ctx->expr->type == EXPRTK_NODE_BLOCK;
     int empty_block = is_block && ctx->expr->data.block.count == 0;
     if (!empty_block) {
       print_repl_val(&res, ctx);
     }
  }

  return ctx->env.aborted ? -1 : 0;
}

int turbo_script_run_and_print(turbo_script_ctx_t *ctx, const char *script) {
  /* In DEBUG mode, use interpreter with result printing */
#ifdef DEBUG
  return turbo_script_repl_run(ctx, script);
#else
  /* In production mode, JIT doesn't support result printing yet */
  /* TODO: implement result capture for JIT mode */
  return turbo_script_run(ctx, script);
#endif
}


turbo_script_compiled_t *turbo_script_compile(turbo_script_ctx_t *ctx, const char *script) {
  if (!ctx || !script)
    return NULL;

  exprtk_node_t *ast = exprtk_parse(script, 0);
  if (!ast) {
    set_error_msg(ctx, "Parse error");
    return NULL;
  }

  if (exprtk_validate(ast, &ctx->env, ctx->error_msg, sizeof(ctx->error_msg)) != 0) {
    exprtk_free(ast);
    return NULL;
  }

  turbo_script_compiled_t *compiled =
      (turbo_script_compiled_t *)malloc(sizeof(turbo_script_compiled_t));
  if (!compiled) {
    exprtk_free(ast);
    return NULL;
  }
  compiled->ast = ast;
  return compiled;
}

int turbo_script_exec(turbo_script_ctx_t *ctx, turbo_script_compiled_t *compiled) {
  if (!ctx || !compiled || !compiled->ast)
    return -1;

  mem_reset(&ctx->scratch_arena);
  ctx->env.aborted = 0;

  exprtk_value_t res =  exprtk_eval(compiled->ast, &ctx->env);
  (void)res;

  return ctx->env.aborted ? -1 : 0;
}

  
bool turbo_script_value_as_bool(exprtk_value_t val) {
  switch (val.type) {
    case EXPRTK_VAL_NUMBER:
      return fabs(val.data.number) > 1e-9;
    case EXPRTK_VAL_STRING:
      return val.data.string.len > 0;
    case EXPRTK_VAL_VECTOR:
      return val.data.vector.size > 0;
    case EXPRTK_VAL_MAP:
      return exprtk_map_count(&val) > 0;
    case EXPRTK_VAL_LIST:
      return val.data.list.count > 0;
    case EXPRTK_VAL_NULL:
      return false;
    case EXPRTK_VAL_FUNCTION:
      return true;
    default:
      return false;
  }
}


void turbo_script_compiled_free(turbo_script_compiled_t *compiled) {
  if (!compiled)
    return;
  if (compiled->ast)
    exprtk_free(compiled->ast);
  free(compiled);
}

int turbo_script_run_file(turbo_script_ctx_t *ctx, const char *filename) {
  if (!ctx || !filename)
    return -1;

  turbo_fs_buf_t buf;
  if (turbo_fs_read_file(filename, &buf) != 0)
    return -1;

  /* Ensure null-termination */
  char *script = (char *)malloc(buf.len + 1);
  if (!script) {
    turbo_fs_buf_free(&buf);
    return -1;
  }

  memcpy(script, buf.base, buf.len);
  script[buf.len] = '\0';
  turbo_fs_buf_free(&buf);

  int ret = turbo_script_run(ctx, script);
  free(script);
  return ret;
}

void bind_num(turbo_script_ctx_t *ctx, const char *name, double value) {
  if (!ctx)
    return;
  exprtk_value_t v = {EXPRTK_VAL_NUMBER, .data.number = value};
  exprtk_env_set(&ctx->env, name, v);
}

void bind_str(turbo_script_ctx_t *ctx, const char *name, const char *value) {
  if (!ctx || !name || !value)
    return;
  char *buf = arena_cstr(&ctx->env.arena, tstr_v_from_cstr(value));
  if (buf) {
    size_t len = strlen(buf);
    exprtk_value_t v = {EXPRTK_VAL_STRING, .data.string = tstr_v_from_buf(buf, len)};
    exprtk_env_set(&ctx->env, name, v);
  }
}

double get_num(turbo_script_ctx_t *ctx, const char *name) {
  if (!ctx)
    return 0.0;
  exprtk_value_t v = exprtk_env_get(&ctx->env, name);
  if (v.type == EXPRTK_VAL_NUMBER)
    return v.data.number;
  return 0.0;
}

const char *turbo_script_get_error(turbo_script_ctx_t *ctx) { return ctx ? ctx->error_msg : ""; }

int bind_vec(turbo_script_ctx_t *ctx, const char *name, const double *data, size_t len) {
  if (!ctx || !name || !data || len == 0)
    return -1;
  double *buf = (double *)mem_alloc(&ctx->env.arena, len * sizeof(double));
  if (!buf)
    return -1;
  memcpy(buf, data, len * sizeof(double));
  exprtk_value_t v = {EXPRTK_VAL_VECTOR, .data.vector = {buf, len}};
  exprtk_env_set(&ctx->env, name, v);
  return 0;
}

int get_vec(turbo_script_ctx_t *ctx, const char *name, const double **data, size_t *len) {
  if (!ctx || !name || !data || !len)
    return -1;
  exprtk_value_t v = exprtk_env_get(&ctx->env, name);
  if (v.type != EXPRTK_VAL_VECTOR)
    return -1;
  *data = v.data.vector.data;
  *len = v.data.vector.size;
  return 0;
}

const char *get_str(turbo_script_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return NULL;
  exprtk_value_t v = exprtk_env_get(&ctx->env, name);
  if (v.type != EXPRTK_VAL_STRING)
    return NULL;
  return v.data.string.data;
}

void bind_func(turbo_script_ctx_t *ctx, const char *name, turbo_script_func_t fn, void *user_data) {
  if (!ctx || !name || !fn)
    return;
  exprtk_env_register_func(&ctx->env, name, fn, user_data);
}
