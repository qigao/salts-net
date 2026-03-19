#include "exprtk.h"
#include "exprtk_module.h"
#include "object_pool.h"
#include "turbo_script.h"
#include "turbo_script_internal.h"

#include "exprtk_grammar.h"
#include <math.h>
#include <mir-gen.h>
#include <mir.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>

/* =========================================================================
 * Phase 1-5: Extended MIR JIT Compiler for TurboScript
 * ========================================================================= */

/* --- Loop stack for break/continue (Phase 1) --- */
#define MAX_LOOP_DEPTH 32

typedef struct {
  MIR_label_t break_label;
  MIR_label_t continue_label;
} loop_frame_t;

/* --- External call items cache (Phase 2) --- */
typedef struct {
  MIR_item_t fmod_proto;
  MIR_item_t fmod_import;
  MIR_item_t pow_proto;
  MIR_item_t pow_import;
  /*  variable bridge */
  MIR_item_t load_var_proto;
  MIR_item_t load_var_import;
  MIR_item_t store_var_proto;
  MIR_item_t store_var_import;
  /*  function call bridges */
  MIR_item_t call0_proto;
  MIR_item_t call0_import;
  MIR_item_t call1_proto;
  MIR_item_t call1_import;
  MIR_item_t call2_proto;
  MIR_item_t call2_import;
  MIR_item_t call3_proto;
  MIR_item_t call3_import;
  MIR_item_t calln_proto;
  MIR_item_t calln_import;
  /*  interpreter fallback */
  MIR_item_t eval_node_proto;
  MIR_item_t eval_node_import;
  /*  vector indexing */
  MIR_item_t vec_get_proto;
  MIR_item_t vec_get_import;
  /*  function definition */
  MIR_item_t define_func_proto;
  MIR_item_t define_func_import;
  /*  member access */
  MIR_item_t member_get_proto;
  MIR_item_t member_get_import;
  /*  vector data pointer */
  MIR_item_t vec_data_proto;
  MIR_item_t vec_data_import;
  /*  map access by key */
  MIR_item_t map_get_key_proto;
  MIR_item_t map_get_key_import;
  /*  map field pointer (for native direct load) */
  MIR_item_t map_num_ptr_proto;
  MIR_item_t map_num_ptr_import;
  /*  direct math function imports (bypass call bridge) */
  MIR_item_t sin_proto, sin_import;
  MIR_item_t cos_proto, cos_import;
  MIR_item_t sqrt_proto, sqrt_import;
  MIR_item_t fabs_proto, fabs_import;
  MIR_item_t floor_proto, floor_import;
  MIR_item_t ceil_proto, ceil_import;
  MIR_item_t log_proto, log_import;
  MIR_item_t exp_proto, exp_import;
  MIR_item_t round_proto, round_import;
  MIR_item_t tan_proto, tan_import;
  MIR_item_t asin_proto, asin_import;
  MIR_item_t acos_proto, acos_import;
  MIR_item_t atan_proto, atan_import;
  MIR_item_t fmax_proto, fmax_import;
  MIR_item_t fmin_proto, fmin_import;
  MIR_item_t atan2_proto, atan2_import;
  /*  direct native/builtin function dispatch */
  MIR_item_t call_native_proto, call_native_import;
  MIR_item_t call_builtin_proto, call_builtin_import;
} ts_mir_externals_t;

/* Compiled script function table entry */
#define MAX_COMPILED_FUNCS 64
typedef struct {
  char *name;
  MIR_item_t mir_func;
  MIR_item_t proto;
  size_t arg_count;
} ts_compiled_func_t;

typedef struct {
  char *name;
  MIR_reg_t reg;
} ts_mir_var_entry_t;

typedef struct {
  MIR_context_t ctx;
  MIR_item_t func;
  MIR_module_t module;
  turbo_script_ctx_t *ts_ctx;

  ts_mir_var_entry_t **vars;
  int var_count;
  int var_capacity;
  int tmp_count;
  int failed;
  object_pool_t *var_pool;

  /*  loop stack for break/continue */
  loop_frame_t loop_stack[MAX_LOOP_DEPTH];
  int loop_depth;

  /*  external call items */
  ts_mir_externals_t ext;

  /*  ctx_ptr register (first function argument) */
  MIR_reg_t ctx_reg;

  /*  compiled script functions */
  ts_compiled_func_t compiled_funcs[MAX_COMPILED_FUNCS];
  int compiled_func_count;

/*  cached vector data pointers for native indexing */
#define MAX_VEC_PTRS 32
  struct {
    const char *name;
    MIR_reg_t ptr_reg;
  } vec_ptrs[32];
  int vec_ptr_count;

/*  cached map field pointers for native access */
#define MAX_MAP_PTRS 64
  struct {
    const char *obj_name;
    const char *key_name;
    MIR_reg_t ptr_reg;
  } map_ptrs[64];
  int map_ptr_count;
} ts_mir_compiler_t;

static void ts_mir_fail(ts_mir_compiler_t *c, const char *fmt, ...) {
  va_list args;

  if (!c || c->failed)
    return;

  c->failed = 1;
  if (!c->ts_ctx)
    return;

  c->ts_ctx->error_code = TURBO_SCRIPT_ERROR_JIT;
  va_start(args, fmt);
  vsnprintf(c->ts_ctx->error_msg, sizeof(c->ts_ctx->error_msg), fmt, args);
  va_end(args);
}

static MIR_reg_t new_temp_reg(ts_mir_compiler_t *c) {
  char name[32];
  snprintf(name, sizeof(name), "_t%d", c->tmp_count++);
  return MIR_new_func_reg(c->ctx, c->func->u.func, MIR_T_D, name);
}

static MIR_reg_t new_temp_ireg(ts_mir_compiler_t *c) {
  char name[32];
  snprintf(name, sizeof(name), "_i%d", c->tmp_count++);
  return MIR_new_func_reg(c->ctx, c->func->u.func, MIR_T_I64, name);
}

static MIR_reg_t new_temp_preg(ts_mir_compiler_t *c) {
  char name[32];
  snprintf(name, sizeof(name), "_p%d", c->tmp_count++);
  return MIR_new_func_reg(c->ctx, c->func->u.func, MIR_T_P, name);
}

static int ts_mir_ensure_var_capacity(ts_mir_compiler_t *c, int needed) {
  ts_mir_var_entry_t **new_vars = NULL;
  int new_capacity = 0;

  if (needed <= c->var_capacity)
    return 1;

  new_capacity = c->var_capacity > 0 ? c->var_capacity * 2 : 32;
  if (new_capacity < needed)
    new_capacity = needed;

  new_vars = (ts_mir_var_entry_t **)realloc(c->vars, (size_t)new_capacity * sizeof(*new_vars));
  if (!new_vars) {
    ts_mir_fail(c, "JIT compile error: out of memory growing variable table");
    return 0;
  }

  c->vars = new_vars;
  c->var_capacity = new_capacity;
  return 1;
}

static MIR_reg_t get_or_create_reg(ts_mir_compiler_t *c, const char *name) {
  ts_mir_var_entry_t *entry = NULL;

  for (int i = 0; i < c->var_count; i++) {
    if (strcmp(c->vars[i]->name, name) == 0) return c->vars[i]->reg;
  }

  if (!ts_mir_ensure_var_capacity(c, c->var_count + 1))
    return new_temp_reg(c);

  entry = (ts_mir_var_entry_t *)object_pool_alloc(c->var_pool);
  if (!entry) {
    ts_mir_fail(c, "JIT compile error: out of memory allocating variable entry for '%s'",
                name ? name : "<unnamed>");
    return new_temp_reg(c);
  }

  entry->name = strdup(name);
  if (!entry->name) {
    object_pool_free(c->var_pool, entry);
    ts_mir_fail(c, "JIT compile error: out of memory duplicating variable name '%s'",
                name ? name : "<unnamed>");
    return new_temp_reg(c);
  }
  entry->reg = MIR_new_func_reg(c->ctx, c->func->u.func, MIR_T_D, name);
  c->vars[c->var_count++] = entry;
  return entry->reg;
}

static void ts_mir_discard_current_vars(ts_mir_compiler_t *c) {
  if (!c)
    return;
  free(c->vars);
  c->vars = NULL;
  c->var_count = 0;
  c->var_capacity = 0;
}

static void ts_mir_destroy_var_pool(ts_mir_compiler_t *c) {
  if (!c)
    return;
  ts_mir_discard_current_vars(c);
  if (c->var_pool) {
    object_pool_destroy(c->var_pool);
    c->var_pool = NULL;
  }
}

static int ts_mir_bind_existing_reg(ts_mir_compiler_t *c, const char *name, MIR_reg_t reg) {
  ts_mir_var_entry_t *entry = NULL;

  if (!ts_mir_ensure_var_capacity(c, c->var_count + 1))
    return 0;

  entry = (ts_mir_var_entry_t *)object_pool_alloc(c->var_pool);
  if (!entry) {
    ts_mir_fail(c, "JIT compile error: out of memory allocating parameter entry for '%s'",
                name ? name : "<unnamed>");
    return 0;
  }

  entry->name = strdup(name);
  if (!entry->name) {
    object_pool_free(c->var_pool, entry);
    ts_mir_fail(c, "JIT compile error: out of memory duplicating parameter name '%s'",
                name ? name : "<unnamed>");
    return 0;
  }

  entry->reg = reg;
  c->vars[c->var_count++] = entry;
  return 1;
}

/* Forward declarations */
static MIR_reg_t ts_compile_expr(ts_mir_compiler_t *c, exprtk_node_t *node);
static void ts_compile_stmt(ts_mir_compiler_t *c, exprtk_node_t *node);
static void ts_compile_branch_false(ts_mir_compiler_t *c, exprtk_node_t *node,
                                    MIR_label_t false_label);
static void ts_compile_branch_true(ts_mir_compiler_t *c, exprtk_node_t *node,
                                   MIR_label_t true_label);
static MIR_reg_t ts_emit_packed_args(ts_mir_compiler_t *c, size_t argc, const MIR_reg_t *arg_regs);
static int ts_emit_direct_math_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                    const MIR_reg_t *arg_regs, MIR_reg_t res);
static int ts_emit_direct_resolved_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                        const MIR_reg_t *arg_regs, MIR_reg_t res);
static void ts_emit_interpreter_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                     const MIR_reg_t *arg_regs, MIR_reg_t res);
static MIR_reg_t ts_emit_member_access(ts_mir_compiler_t *c, const char *obj_name,
                                       const char *member);
static void ts_emit_sync_to_env(ts_mir_compiler_t *c);
static void ts_emit_reload_from_env(ts_mir_compiler_t *c);
static MIR_reg_t ts_emit_eval_node(ts_mir_compiler_t *c, exprtk_node_t *node, int full_sync);

typedef struct {
  const char *name;
  size_t proto_offset;
  size_t import_offset;
} ts_math_dispatch_entry_t;

static int ts_math_dispatch_cmp(const void *key, const void *entry) {
  return strcmp((const char *)key, ((const ts_math_dispatch_entry_t *)entry)->name);
}

static int ts_find_math_dispatch(ts_mir_compiler_t *c, const char *name, size_t argc,
                                 MIR_item_t *proto, MIR_item_t *import) {
  static const ts_math_dispatch_entry_t unary_table[] = {
      {"abs", offsetof(ts_mir_externals_t, fabs_proto), offsetof(ts_mir_externals_t, fabs_import)},
      {"acos", offsetof(ts_mir_externals_t, acos_proto),
       offsetof(ts_mir_externals_t, acos_import)},
      {"asin", offsetof(ts_mir_externals_t, asin_proto),
       offsetof(ts_mir_externals_t, asin_import)},
      {"atan", offsetof(ts_mir_externals_t, atan_proto),
       offsetof(ts_mir_externals_t, atan_import)},
      {"ceil", offsetof(ts_mir_externals_t, ceil_proto),
       offsetof(ts_mir_externals_t, ceil_import)},
      {"cos", offsetof(ts_mir_externals_t, cos_proto), offsetof(ts_mir_externals_t, cos_import)},
      {"exp", offsetof(ts_mir_externals_t, exp_proto), offsetof(ts_mir_externals_t, exp_import)},
      {"floor", offsetof(ts_mir_externals_t, floor_proto),
       offsetof(ts_mir_externals_t, floor_import)},
      {"log", offsetof(ts_mir_externals_t, log_proto), offsetof(ts_mir_externals_t, log_import)},
      {"round", offsetof(ts_mir_externals_t, round_proto),
       offsetof(ts_mir_externals_t, round_import)},
      {"sin", offsetof(ts_mir_externals_t, sin_proto), offsetof(ts_mir_externals_t, sin_import)},
      {"sqrt", offsetof(ts_mir_externals_t, sqrt_proto),
       offsetof(ts_mir_externals_t, sqrt_import)},
      {"tan", offsetof(ts_mir_externals_t, tan_proto), offsetof(ts_mir_externals_t, tan_import)},
  };
  static const ts_math_dispatch_entry_t binary_table[] = {
      {"atan2", offsetof(ts_mir_externals_t, atan2_proto),
       offsetof(ts_mir_externals_t, atan2_import)},
      {"max", offsetof(ts_mir_externals_t, fmax_proto),
       offsetof(ts_mir_externals_t, fmax_import)},
      {"min", offsetof(ts_mir_externals_t, fmin_proto),
       offsetof(ts_mir_externals_t, fmin_import)},
  };

  const ts_math_dispatch_entry_t *table = NULL;
  size_t table_count = 0;
  const ts_math_dispatch_entry_t *entry = NULL;
  char *ext_base = NULL;

  if (!c || !name || !proto || !import)
    return 0;

  if (argc == 1) {
    table = unary_table;
    table_count = sizeof(unary_table) / sizeof(unary_table[0]);
  } else if (argc == 2) {
    table = binary_table;
    table_count = sizeof(binary_table) / sizeof(binary_table[0]);
  } else {
    return 0;
  }

  entry = (const ts_math_dispatch_entry_t *)bsearch(name, table, table_count, sizeof(table[0]),
                                                    ts_math_dispatch_cmp);
  if (!entry)
    return 0;

  ext_base = (char *)&c->ext;
  *proto = *(MIR_item_t *)(ext_base + entry->proto_offset);
  *import = *(MIR_item_t *)(ext_base + entry->import_offset);
  return 1;
}
static size_t ts_compile_call_args(ts_mir_compiler_t *c, size_t argc, exprtk_node_t **args,
                                   MIR_reg_t out_regs[16]);
static MIR_reg_t ts_emit_index_access(ts_mir_compiler_t *c, const char *name,
                                      exprtk_node_t *index_node);
static int ts_is_non_numeric_node(exprtk_node_t *node);
static MIR_reg_t ts_emit_assignment(ts_mir_compiler_t *c, exprtk_node_t *node);
static int ts_compile_data_access_and_assignment(ts_mir_compiler_t *c, exprtk_node_t *node,
                                                 MIR_reg_t *out);

/* --- Dispatch helpers: function calls (Phase 13/17 + fallback) --- */
static MIR_reg_t ts_emit_packed_args(ts_mir_compiler_t *c, size_t argc, const MIR_reg_t *arg_regs) {
  MIR_reg_t arr_reg = new_temp_preg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_ALLOCA, MIR_new_reg_op(c->ctx, arr_reg),
                               MIR_new_int_op(c->ctx, (int64_t)(argc * sizeof(double)))));
  for (size_t i = 0; i < argc; i++) {
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_insn(c->ctx, MIR_DMOV,
                     MIR_new_mem_op(c->ctx, MIR_T_D, (int64_t)(i * sizeof(double)), arr_reg, 0, 1),
                     MIR_new_reg_op(c->ctx, arg_regs[i])));
  }
  return arr_reg;
}

static int ts_emit_direct_math_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                    const MIR_reg_t *arg_regs, MIR_reg_t res) {
  MIR_item_t proto = NULL, import = NULL;

  if (argc == 1) {
    if (!ts_find_math_dispatch(c, name, argc, &proto, &import)) return 0;
    if (!proto) return 0;
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 4, MIR_new_ref_op(c->ctx, proto),
                                      MIR_new_ref_op(c->ctx, import), MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, arg_regs[0])));
    return 1;
  }

  if (argc == 2) {
    if (!ts_find_math_dispatch(c, name, argc, &proto, &import)) return 0;
    if (!proto) return 0;
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, proto),
                                      MIR_new_ref_op(c->ctx, import), MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, arg_regs[0]),
                                      MIR_new_reg_op(c->ctx, arg_regs[1])));
    return 1;
  }

  return 0;
}

static int ts_emit_direct_resolved_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                        const MIR_reg_t *arg_regs, MIR_reg_t res) {
  /* 1) env-registered native functions */
  exprtk_func_t *f = c->ts_ctx->env.funcs;
  while (f) {
    if (f->name && strcmp(f->name, name) == 0 && !f->is_script) {
      void *fn_ptr = (void *)f->data.native.fn;
      void *ud = f->data.native.user_data;
      MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_call_insn(
                          c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.call_native_proto),
                          MIR_new_ref_op(c->ctx, c->ext.call_native_import),
                          MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)fn_ptr),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)ud),
                          MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
      return 1;
    }
    f = f->next;
  }

  /* 2) module/registry builtins */
  exprtk_builtin_fn bfn = exprtk_find_builtin(name, &c->ts_ctx->env);
  if (bfn) {
    MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.call_builtin_proto),
                          MIR_new_ref_op(c->ctx, c->ext.call_builtin_import),
                          MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)bfn),
                          MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
    return 1;
  }

  return 0;
}

static void ts_emit_interpreter_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                     const MIR_reg_t *arg_regs, MIR_reg_t res) {
  if (argc == 0) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.call0_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.call0_import),
                                      MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name)));
  } else if (argc == 1) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.call1_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.call1_import),
                                      MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                                      MIR_new_reg_op(c->ctx, arg_regs[0])));
  } else if (argc == 2) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(
                        c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.call2_proto),
                        MIR_new_ref_op(c->ctx, c->ext.call2_import), MIR_new_reg_op(c->ctx, res),
                        MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                        MIR_new_reg_op(c->ctx, arg_regs[0]), MIR_new_reg_op(c->ctx, arg_regs[1])));
  } else if (argc == 3) {
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.call3_proto),
                          MIR_new_ref_op(c->ctx, c->ext.call3_import), MIR_new_reg_op(c->ctx, res),
                          MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                          MIR_new_reg_op(c->ctx, arg_regs[0]), MIR_new_reg_op(c->ctx, arg_regs[1]),
                          MIR_new_reg_op(c->ctx, arg_regs[2])));
  } else {
    MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.calln_proto),
                          MIR_new_ref_op(c->ctx, c->ext.calln_import), MIR_new_reg_op(c->ctx, res),
                          MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                          MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  }
}

/* --- Access helpers: member/index fast path + bridge fallback --- */
static MIR_reg_t ts_emit_member_access(ts_mir_compiler_t *c, const char *obj_name,
                                       const char *member) {
  exprtk_value_t obj = exprtk_env_get(&c->ts_ctx->env, obj_name);
  if (obj.type == EXPRTK_VAL_MAP && exprtk_map_has(&obj, member)) {
    MIR_reg_t ptr_reg = 0;
    for (int i = 0; i < c->map_ptr_count; i++) {
      if (c->map_ptrs[i].obj_name == obj_name && strcmp(c->map_ptrs[i].key_name, member) == 0) {
        ptr_reg = c->map_ptrs[i].ptr_reg;
        break;
      }
    }
    if (!ptr_reg && c->map_ptr_count < MAX_MAP_PTRS) {
      ptr_reg = new_temp_ireg(c);
      c->map_ptrs[c->map_ptr_count].obj_name = obj_name;
      c->map_ptrs[c->map_ptr_count].key_name = member;
      c->map_ptrs[c->map_ptr_count].ptr_reg = ptr_reg;
      c->map_ptr_count++;
    }

    if (ptr_reg) {
      MIR_reg_t res = new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                   MIR_new_mem_op(c->ctx, MIR_T_D, 0, ptr_reg, 0, 1)));
      return res;
    }

    MIR_reg_t res = new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.map_get_key_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.map_get_key_import),
                                      MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)obj_name),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member)));
    return res;
  }

  MIR_reg_t res = new_temp_reg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.member_get_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.member_get_import),
                                    MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)obj_name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member)));
  return res;
}

static void ts_emit_sync_to_env(ts_mir_compiler_t *c) {
  for (int i = 0; i < c->var_count; i++) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.store_var_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.store_var_import),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name),
                                      MIR_new_reg_op(c->ctx, c->vars[i]->reg)));
  }
}

static void ts_emit_reload_from_env(ts_mir_compiler_t *c) {
  for (int i = 0; i < c->var_count; i++) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(
                        c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.load_var_proto),
                        MIR_new_ref_op(c->ctx, c->ext.load_var_import),
                        MIR_new_reg_op(c->ctx, c->vars[i]->reg),
                        MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name)));
  }
}

static MIR_reg_t ts_emit_eval_node(ts_mir_compiler_t *c, exprtk_node_t *node, int full_sync) {
  if (full_sync) ts_emit_sync_to_env(c);

  MIR_reg_t res = new_temp_reg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.eval_node_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.eval_node_import),
                                    MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));

  if (full_sync) ts_emit_reload_from_env(c);
  return res;
}

static size_t ts_compile_call_args(ts_mir_compiler_t *c, size_t argc, exprtk_node_t **args,
                                   MIR_reg_t out_regs[16]) {
  if (argc > 16) argc = 16;
  for (size_t i = 0; i < argc; i++) {
    out_regs[i] = ts_compile_expr(c, args[i]);
  }
  return argc;
}

static MIR_reg_t ts_emit_index_access(ts_mir_compiler_t *c, const char *name,
                                      exprtk_node_t *index_node) {
  MIR_reg_t idx_reg = ts_compile_expr(c, index_node);

  /* Native fast path for vectors pre-bound in env at compile time. */
  exprtk_value_t existing = exprtk_env_get(&c->ts_ctx->env, name);
  int is_prebound = (existing.type == EXPRTK_VAL_VECTOR && existing.data.vector.data != NULL);
  if (is_prebound) {
    MIR_reg_t ptr_reg = 0;
    for (int i = 0; i < c->vec_ptr_count; i++) {
      if (strcmp(c->vec_ptrs[i].name, name) == 0) {
        ptr_reg = c->vec_ptrs[i].ptr_reg;
        break;
      }
    }
    if (!ptr_reg && c->vec_ptr_count < MAX_VEC_PTRS) {
      ptr_reg = new_temp_ireg(c);
      c->vec_ptrs[c->vec_ptr_count].name = name;
      c->vec_ptrs[c->vec_ptr_count].ptr_reg = ptr_reg;
      c->vec_ptr_count++;
    }

    if (ptr_reg) {
      MIR_reg_t idx_i = new_temp_ireg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_D2I, MIR_new_reg_op(c->ctx, idx_i),
                                   MIR_new_reg_op(c->ctx, idx_reg)));

      MIR_reg_t res = new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                   MIR_new_mem_op(c->ctx, MIR_T_D, 0, ptr_reg, idx_i, 8)));
      return res;
    }
  }

  /* Bridge fallback: vec_get(ctx, name, idx). */
  MIR_reg_t res = new_temp_reg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.vec_get_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.vec_get_import),
                                    MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                                    MIR_new_reg_op(c->ctx, idx_reg)));
  return res;
}

/* --- Assignment/fallback helpers: env sync + interpreter eval --- */
static int ts_is_non_numeric_node(exprtk_node_t *node) {
  if (!node) return 0;
  return node->type == EXPRTK_NODE_VECTOR || node->type == EXPRTK_NODE_MAP_LITERAL ||
         node->type == EXPRTK_NODE_STRING || node->type == EXPRTK_NODE_TEMPLATE_STRING ||
         node->type == EXPRTK_NODE_SLICE;
}

static MIR_reg_t ts_emit_assignment(ts_mir_compiler_t *c, exprtk_node_t *node) {
  exprtk_node_t *rhs = node->data.assignment.value;
  if (ts_is_non_numeric_node(rhs)) {
    /* Keep interpreter as source-of-truth for complex value assignment. */
    return ts_emit_eval_node(c, node, 1);
  }

  MIR_reg_t val = ts_compile_expr(c, rhs);
  MIR_reg_t target = get_or_create_reg(c, node->data.assignment.name);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target), MIR_new_reg_op(c->ctx, val)));
  return target;
}

static int ts_compile_data_access_and_assignment(ts_mir_compiler_t *c, exprtk_node_t *node,
                                                 MIR_reg_t *out) {
  if (!node || !out) return 0;

  switch (node->type) {
  case EXPRTK_NODE_ASSIGNMENT:
    *out = ts_emit_assignment(c, node);
    return 1;

  case EXPRTK_NODE_CONSTANT_DECL: {
    MIR_reg_t val = ts_compile_expr(c, node->data.assignment.value);
    MIR_reg_t target = get_or_create_reg(c, node->data.assignment.name);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                                 MIR_new_reg_op(c->ctx, val)));
    *out = target;
    return 1;
  }

  case EXPRTK_NODE_NULL: {
    MIR_reg_t r = new_temp_reg(c);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r), MIR_new_double_op(c->ctx, 0.0)));
    *out = r;
    return 1;
  }

  case EXPRTK_NODE_INDEX:
    if (node->data.index_access.array &&
        node->data.index_access.array->type == EXPRTK_NODE_VARIABLE && c->ctx_reg) {
      const char *name = node->data.index_access.array->data.variable.name;
      *out = ts_emit_index_access(c, name, node->data.index_access.index);
    } else {
      *out = ts_emit_eval_node(c, node, 1);
    }
    return 1;

  case EXPRTK_NODE_MEMBER_ACCESS:
    if (node->data.member_access.object &&
        node->data.member_access.object->type == EXPRTK_NODE_VARIABLE &&
        node->data.member_access.member) {
      const char *obj_name = node->data.member_access.object->data.variable.name;
      const char *member = node->data.member_access.member;
      *out = ts_emit_member_access(c, obj_name, member);
    } else {
      *out = ts_emit_eval_node(c, node, 1);
    }
    return 1;

  default:
    return 0;
  }
}

/* =========================================================================
 *  Lookup compiled function by name
 * ========================================================================= */

static ts_compiled_func_t *ts_find_compiled_func(ts_mir_compiler_t *c, const char *name) {
  for (int i = 0; i < c->compiled_func_count; i++) {
    if (strcmp(c->compiled_funcs[i].name, name) == 0) return &c->compiled_funcs[i];
  }
  return NULL;
}

/* Compile a script function as a separate MIR function.
 * Signature: double func_name(double arg0, double arg1, ...)
 * MUST be called when no other MIR function is open. */
static void ts_compile_script_func(ts_mir_compiler_t *c, const char *name,
                                   exprtk_node_t **arg_params, size_t arg_count,
                                   exprtk_node_t *body) {
  if (c->compiled_func_count >= MAX_COMPILED_FUNCS) return;
  if (arg_count > 16) return; /* sanity limit */

  /* Save parent compiler state */
  MIR_item_t saved_func = c->func;
  int saved_var_count = c->var_count;
  int saved_var_capacity = c->var_capacity;
  int saved_tmp_count = c->tmp_count;
  int saved_loop_depth = c->loop_depth;
  MIR_reg_t saved_ctx_reg = c->ctx_reg;
  ts_mir_var_entry_t **saved_vars = c->vars;

  /* Reset compiler state for the new function */
  c->vars = NULL;
  c->var_count = 0;
  c->var_capacity = 0;
  c->tmp_count = 0;
  c->loop_depth = 0;
  c->ctx_reg = 0; /* No ctx_ptr in compiled functions — disables var sync in return */

  /* Create unique function name */
  char func_name[128];
  snprintf(func_name, sizeof(func_name), "ts_%s", name);

  /* Build MIR function: double func_name(double a0, double a1, ...) */
  MIR_type_t res_type = MIR_T_D;
  MIR_var_t mir_args[16];
  for (size_t i = 0; i < arg_count; i++) {
    mir_args[i].type = MIR_T_D;
    mir_args[i].name = arg_params[i]->data.variable.name;
    mir_args[i].size = 0;
  }

  MIR_item_t new_func =
      MIR_new_func_arr(c->ctx, func_name, 1, &res_type, (uint32_t)arg_count, mir_args);
  c->func = new_func;

  /* Map parameter names to their MIR registers */
  for (size_t i = 0; i < arg_count; i++) {
    const char *pname = arg_params[i]->data.variable.name;
    if (!ts_mir_bind_existing_reg(c, pname, MIR_reg(c->ctx, pname, new_func->u.func))) break;
  }

  /* Compile the function body */
  ts_compile_stmt(c, body);

  /* Default return 0.0 (in case body doesn't return) */
  MIR_reg_t ret_reg = new_temp_reg(c);
  MIR_append_insn(c->ctx, new_func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, ret_reg),
                               MIR_new_double_op(c->ctx, 0.0)));
  MIR_append_insn(c->ctx, new_func, MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, ret_reg)));

  MIR_finish_func(c->ctx);

  /* Create a proto for calling this function */
  char proto_name[128];
  snprintf(proto_name, sizeof(proto_name), "p_ts_%s", name);
  MIR_item_t proto =
      MIR_new_proto_arr(c->ctx, proto_name, 1, &res_type, (uint32_t)arg_count, mir_args);

  /* Register in compiled function table */
  int idx = c->compiled_func_count++;
  c->compiled_funcs[idx].name = strdup(name);
  c->compiled_funcs[idx].mir_func = new_func;
  c->compiled_funcs[idx].proto = proto;
  c->compiled_funcs[idx].arg_count = arg_count;

  /* Restore parent compiler state */
  free(c->vars);
  c->func = saved_func;
  c->vars = saved_vars;
  c->var_count = saved_var_count;
  c->var_capacity = saved_var_capacity;
  c->tmp_count = saved_tmp_count;
  c->loop_depth = saved_loop_depth;
  c->ctx_reg = saved_ctx_reg;
}

/* Pre-scan AST for function definitions and compile them as MIR functions.
 * Must be called BEFORE the main function is created. */
static void ts_prescan_functions(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!node) return;

  if (node->type == EXPRTK_NODE_FUNCTION_DEFINITION) {
    if (node->data.func_def.name && node->data.func_def.body &&
        node->data.func_def.arg_count <= 16) {
      int all_vars = 1;
      for (size_t i = 0; i < node->data.func_def.arg_count; i++) {
        if (node->data.func_def.arg_params[i]->type != EXPRTK_NODE_VARIABLE) {
          all_vars = 0;
          break;
        }
      }
      if (all_vars) {
        ts_compile_script_func(c, node->data.func_def.name, node->data.func_def.arg_params,
                               node->data.func_def.arg_count, node->data.func_def.body);
      }
    }
  }

  /* Recurse into blocks to find nested function definitions */
  if (node->type == EXPRTK_NODE_BLOCK) {
    for (size_t i = 0; i < node->data.block.count; i++) {
      ts_prescan_functions(c, node->data.block.statements[i]);
    }
  }
}

/* =========================================================================
 *  Variable Bridge Functions
 * ========================================================================= */

double ts_mir_load_var(void *ctx_ptr, const char *name) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val = exprtk_env_get(&ctx->env, name);
  return (val.type == EXPRTK_VAL_NUMBER) ? val.data.number : 0.0;
}

void ts_mir_store_var(void *ctx_ptr, const char *name, double value) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val;
  val.type = EXPRTK_VAL_NUMBER;
  val.data.number = value;
  exprtk_env_set(&ctx->env, name, val);
}

/* =========================================================================
 *  Function Call Bridge Functions
 * ========================================================================= */

static exprtk_value_t ts_mir_call_bridge(void *ctx_ptr, const char *name, size_t argc,
                                         double *argv) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t args[16];
  if (argc > 16) argc = 16;
  for (size_t i = 0; i < argc; i++) {
    args[i].type = EXPRTK_VAL_NUMBER;
    args[i].data.number = argv[i];
  }
  return exprtk_call_internal(name, argc, args, &ctx->env, &ctx->env.arena);
}

double ts_mir_call0(void *ctx_ptr, const char *name) {
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, 0, NULL);
  return (r.type == EXPRTK_VAL_NUMBER) ? r.data.number : 0.0;
}

double ts_mir_call1(void *ctx_ptr, const char *name, double a0) {
  double argv[1] = {a0};
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, 1, argv);
  return (r.type == EXPRTK_VAL_NUMBER) ? r.data.number : 0.0;
}

double ts_mir_call2(void *ctx_ptr, const char *name, double a0, double a1) {
  double argv[2] = {a0, a1};
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, 2, argv);
  return (r.type == EXPRTK_VAL_NUMBER) ? r.data.number : 0.0;
}

double ts_mir_call3(void *ctx_ptr, const char *name, double a0, double a1, double a2) {
  double argv[3] = {a0, a1, a2};
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, 3, argv);
  return (r.type == EXPRTK_VAL_NUMBER) ? r.data.number : 0.0;
}

double ts_mir_calln(void *ctx_ptr, const char *name, int64_t argc, double *argv) {
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, (size_t)argc, argv);
  return (r.type == EXPRTK_VAL_NUMBER) ? r.data.number : 0.0;
}

/* =========================================================================
 *  Direct native function dispatch (no strcmp lookup)
 * ========================================================================= */

/* Direct call to exprtk_native_fn — env-registered functions */
double ts_mir_call_native(void *ctx_ptr, void *fn_ptr, void *user_data, int64_t argc,
                          double *argv) {
  (void)ctx_ptr;
  exprtk_native_fn fn = (exprtk_native_fn)fn_ptr;
  exprtk_value_t args[16];
  if (argc > 16) argc = 16;
  for (int64_t i = 0; i < argc; i++) {
    args[i].type = EXPRTK_VAL_NUMBER;
    args[i].data.number = argv[i];
  }
  exprtk_value_t r = fn((size_t)argc, args, user_data);
  return (r.type == EXPRTK_VAL_NUMBER) ? r.data.number : 0.0;
}

/* Direct call to exprtk_builtin_fn — module/registry functions */
double ts_mir_call_builtin(void *ctx_ptr, void *fn_ptr, int64_t argc, double *argv) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_builtin_fn fn = (exprtk_builtin_fn)fn_ptr;
  exprtk_value_t args[16];
  if (argc > 16) argc = 16;
  for (int64_t i = 0; i < argc; i++) {
    args[i].type = EXPRTK_VAL_NUMBER;
    args[i].data.number = argv[i];
  }
  exprtk_value_t r = fn((size_t)argc, args, &ctx->env, &ctx->env.arena);
  return (r.type == EXPRTK_VAL_NUMBER) ? r.data.number : 0.0;
}

/* =========================================================================
 *  Interpreter Fallback Bridge
 * ========================================================================= */

double ts_mir_eval_node(void *ctx_ptr, void *node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *node = (exprtk_node_t *)node_ptr;
  exprtk_value_t result = exprtk_eval(node, &ctx->env);
  return (result.type == EXPRTK_VAL_NUMBER) ? result.data.number : 0.0;
}

/* =========================================================================
 *  Vector Indexing Bridge
 * ========================================================================= */

double ts_mir_vec_get(void *ctx_ptr, const char *name, double index) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val = exprtk_env_get(&ctx->env, name);
  if (val.type == EXPRTK_VAL_VECTOR) {
    int idx = (int)index;
    if (idx >= 0 && idx < (int)val.data.vector.size) return val.data.vector.data[idx];
  }
  return 0.0;
}

/* =========================================================================
 *  Function Definition Bridge
 * ========================================================================= */

void ts_mir_define_func(void *ctx_ptr, void *node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *node = (exprtk_node_t *)node_ptr;
  /* Delegate to interpreter — registers the function in env */
  exprtk_eval(node, &ctx->env);
}

/* =========================================================================
 *  Member Access Bridge (obj.prop → double)
 * ========================================================================= */

double ts_mir_member_get(void *ctx_ptr, const char *obj_name, const char *member) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t obj = exprtk_env_get(&ctx->env, obj_name);
  if (obj.type == EXPRTK_VAL_VECTOR) {
    if (strcmp(member, "length") == 0) return (double)obj.data.vector.size;
  } else if (obj.type == EXPRTK_VAL_STRING) {
    if (strcmp(member, "length") == 0) return (double)obj.data.string.len;
  } else if (obj.type == EXPRTK_VAL_MAP) {
    exprtk_value_t val = exprtk_map_get(&obj, member);
    if (val.type == EXPRTK_VAL_NUMBER) return val.data.number;
  }
  return 0.0;
}

/* =========================================================================
 *  Vector Data Pointer Bridge (for native indexing)
 * ========================================================================= */

void *ts_mir_vec_data(void *ctx_ptr, const char *name) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val = exprtk_env_get(&ctx->env, name);
  if (val.type == EXPRTK_VAL_VECTOR && val.data.vector.data) return (void *)val.data.vector.data;
  return NULL;
}

/* =========================================================================
 *  Map Access by Key (O(1) hash lookup)
 * ========================================================================= */

double ts_mir_map_get_key(void *ctx_ptr, const char *obj_name, const char *key) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t obj = exprtk_env_get(&ctx->env, obj_name);
  if (obj.type == EXPRTK_VAL_MAP) {
    exprtk_value_t val = exprtk_map_get(&obj, key);
    if (val.type == EXPRTK_VAL_NUMBER) return val.data.number;
  }
  return 0.0;
}

/* Returns a pointer to the value's number field inside the HTAB for direct memory access.
 * The pointer is cached in the JIT prologue — one call per field, not per access. */
void *ts_mir_map_num_ptr(void *ctx_ptr, const char *obj_name, const char *key) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t obj = exprtk_env_get(&ctx->env, obj_name);
  if (obj.type == EXPRTK_VAL_MAP) {
    /* We need a stable pointer into the HTAB entry.
     * Use exprtk_map_get_ptr which returns a pointer to the value inside the htab. */
    exprtk_value_t *vp = exprtk_map_get_ptr(&obj, key);
    if (vp && vp->type == EXPRTK_VAL_NUMBER) return (void *)&vp->data.number;
  }
  return NULL;
}

/* =========================================================================
 *  Compile-time constant folding
 * Recursively evaluates constant expression trees, returns 1 if foldable.
 * ========================================================================= */

static int ts_try_fold_constant(exprtk_node_t *node, double *out) {
  if (!node) return 0;

  if (node->type == EXPRTK_NODE_NUMBER) {
    *out = node->data.number;
    return 1;
  }

  if (node->type == EXPRTK_NODE_NULL) {
    *out = 0.0;
    return 1;
  }

  if (node->type == EXPRTK_NODE_BINARY_OP) {
    /* Unary operators */
    if (node->data.binary.left == NULL) {
      double r;
      if (!ts_try_fold_constant(node->data.binary.right, &r)) return 0;
      switch (node->data.binary.op) {
      case exprtk_TOKEN_MINUS:
        *out = -r;
        return 1;
      case exprtk_TOKEN_PLUS:
        *out = r;
        return 1;
      case exprtk_TOKEN_NOT:
        *out = (r == 0.0) ? 1.0 : 0.0;
        return 1;
      default:
        return 0;
      }
    }

    double l, r;
    if (!ts_try_fold_constant(node->data.binary.left, &l)) return 0;
    if (!ts_try_fold_constant(node->data.binary.right, &r)) return 0;

    switch (node->data.binary.op) {
    case exprtk_TOKEN_PLUS:
      *out = l + r;
      return 1;
    case exprtk_TOKEN_MINUS:
      *out = l - r;
      return 1;
    case exprtk_TOKEN_MULTIPLY:
      *out = l * r;
      return 1;
    case exprtk_TOKEN_DIVIDE:
      *out = (r != 0.0) ? l / r : 0.0;
      return 1;
    case exprtk_TOKEN_MOD:
      *out = fmod(l, r);
      return 1;
    case exprtk_TOKEN_POWER:
      *out = pow(l, r);
      return 1;
    case exprtk_TOKEN_LT:
      *out = (l < r) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_GT:
      *out = (l > r) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_LE:
      *out = (l <= r) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_GE:
      *out = (l >= r) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_EQ:
      *out = (l == r) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_NE:
      *out = (l != r) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_AND:
      *out = (l != 0.0 && r != 0.0) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_OR:
      *out = (l != 0.0 || r != 0.0) ? 1.0 : 0.0;
      return 1;
    default:
      return 0;
    }
  }

  return 0;
}

/* =========================================================================
 * Expression Compiler (Phase 1-7)
 * ========================================================================= */

static MIR_reg_t ts_compile_expr(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || c->failed || !node) return 0;

  /*  Try constant folding before anything else */
  {
    double folded;
    if (ts_try_fold_constant(node, &folded)) {
      MIR_reg_t r = new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r),
                                   MIR_new_double_op(c->ctx, folded)));
      return r;
    }
  }

  /* Fast sub-dispatch for data access + assignment nodes. */
  {
    MIR_reg_t specialized = 0;
    if (ts_compile_data_access_and_assignment(c, node, &specialized)) return specialized;
  }

  switch (node->type) {
  case EXPRTK_NODE_NUMBER: {
    MIR_reg_t r = new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r),
                                 MIR_new_double_op(c->ctx, node->data.number)));
    return r;
  }

  case EXPRTK_NODE_VARIABLE:
    return get_or_create_reg(c, node->data.variable.name);

  case EXPRTK_NODE_BINARY_OP: {
    /*  Unary operators (left == NULL) */
    if (node->data.binary.left == NULL) {
      MIR_reg_t operand = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t res = new_temp_reg(c);

      if (node->data.binary.op == exprtk_TOKEN_NOT) {
        /* !x => (x == 0.0) ? 1.0 : 0.0 */
        MIR_reg_t ireg = new_temp_ireg(c);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_DEQ, MIR_new_reg_op(c->ctx, ireg),
                                     MIR_new_reg_op(c->ctx, operand),
                                     MIR_new_double_op(c->ctx, 0.0)));
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res),
                                     MIR_new_reg_op(c->ctx, ireg)));
        return res;
      } else if (node->data.binary.op == exprtk_TOKEN_MINUS) {
        /* unary minus: -x => 0.0 - x */
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_DSUB, MIR_new_reg_op(c->ctx, res),
                                     MIR_new_double_op(c->ctx, 0.0),
                                     MIR_new_reg_op(c->ctx, operand)));
        return res;
      } else if (node->data.binary.op == exprtk_TOKEN_PLUS) {
        /* unary plus: +x => x (no-op) */
        return operand;
      }
      /* unknown unary: just return the operand */
      return operand;
    }

    /*  Logical AND (short-circuit) */
    if (node->data.binary.op == exprtk_TOKEN_AND) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t res = new_temp_reg(c);
      MIR_label_t false_label = MIR_new_label(c->ctx);
      MIR_label_t end_label = MIR_new_label(c->ctx);

      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBEQ, MIR_new_label_op(c->ctx, false_label),
                                   MIR_new_reg_op(c->ctx, left), MIR_new_double_op(c->ctx, 0.0)));

      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t ireg = new_temp_ireg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DNE, MIR_new_reg_op(c->ctx, ireg),
                                   MIR_new_reg_op(c->ctx, right), MIR_new_double_op(c->ctx, 0.0)));
      MIR_append_insn(
          c->ctx, c->func,
          MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, ireg)));
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));

      MIR_append_insn(c->ctx, c->func, false_label);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                   MIR_new_double_op(c->ctx, 0.0)));

      MIR_append_insn(c->ctx, c->func, end_label);
      return res;
    }

    /*  Logical OR (short-circuit) */
    if (node->data.binary.op == exprtk_TOKEN_OR) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t res = new_temp_reg(c);
      MIR_label_t true_label = MIR_new_label(c->ctx);
      MIR_label_t end_label = MIR_new_label(c->ctx);

      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, true_label),
                                   MIR_new_reg_op(c->ctx, left), MIR_new_double_op(c->ctx, 0.0)));

      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t ireg = new_temp_ireg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DNE, MIR_new_reg_op(c->ctx, ireg),
                                   MIR_new_reg_op(c->ctx, right), MIR_new_double_op(c->ctx, 0.0)));
      MIR_append_insn(
          c->ctx, c->func,
          MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, ireg)));
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));

      MIR_append_insn(c->ctx, c->func, true_label);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                   MIR_new_double_op(c->ctx, 1.0)));

      MIR_append_insn(c->ctx, c->func, end_label);
      return res;
    }

    /*  Modulo via fmod() external call */
    if (node->data.binary.op == exprtk_TOKEN_MOD) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t res = new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.fmod_proto),
                                        MIR_new_ref_op(c->ctx, c->ext.fmod_import),
                                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, left),
                                        MIR_new_reg_op(c->ctx, right)));
      return res;
    }

    /*  Power via pow() external call */
    if (node->data.binary.op == exprtk_TOKEN_POWER) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t res = new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.pow_proto),
                                        MIR_new_ref_op(c->ctx, c->ext.pow_import),
                                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, left),
                                        MIR_new_reg_op(c->ctx, right)));
      return res;
    }

    /* Compound assignments */
    MIR_reg_t res = 0;
    switch (node->data.binary.op) {
    case exprtk_TOKEN_ASSIGN_ADD:
    case exprtk_TOKEN_ASSIGN_SUB:
    case exprtk_TOKEN_ASSIGN_MUL:
    case exprtk_TOKEN_ASSIGN_DIV: {
      if (node->data.binary.left->type != EXPRTK_NODE_VARIABLE) return 0;
      MIR_reg_t target = get_or_create_reg(c, node->data.binary.left->data.variable.name);
      MIR_reg_t rhs = ts_compile_expr(c, node->data.binary.right);
      MIR_insn_code_t op;
      if (node->data.binary.op == exprtk_TOKEN_ASSIGN_ADD) op = MIR_DADD;
      else if (node->data.binary.op == exprtk_TOKEN_ASSIGN_SUB) op = MIR_DSUB;
      else if (node->data.binary.op == exprtk_TOKEN_ASSIGN_MUL) op = MIR_DMUL;
      else op = MIR_DDIV;
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, op, MIR_new_reg_op(c->ctx, target),
                                   MIR_new_reg_op(c->ctx, target), MIR_new_reg_op(c->ctx, rhs)));
      return target;
    }
    default: {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_insn_code_t op;
      int is_cmp = 0;
      switch (node->data.binary.op) {
      case exprtk_TOKEN_PLUS:
        op = MIR_DADD;
        break;
      case exprtk_TOKEN_MINUS:
        op = MIR_DSUB;
        break;
      case exprtk_TOKEN_MULTIPLY:
        op = MIR_DMUL;
        break;
      case exprtk_TOKEN_DIVIDE:
        op = MIR_DDIV;
        break;
      case exprtk_TOKEN_LT:
        op = MIR_DLT;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_GT:
        op = MIR_DGT;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_LE:
        op = MIR_DLE;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_GE:
        op = MIR_DGE;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_EQ:
        op = MIR_DEQ;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_NE:
        op = MIR_DNE;
        is_cmp = 1;
        break;
      default:
        op = MIR_DADD;
        break;
      }
      if (is_cmp) {
        MIR_reg_t ireg = new_temp_ireg(c);
        res = new_temp_reg(c);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, op, MIR_new_reg_op(c->ctx, ireg),
                                     MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res),
                                     MIR_new_reg_op(c->ctx, ireg)));
      } else {
        res = new_temp_reg(c);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, op, MIR_new_reg_op(c->ctx, res),
                                     MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
      }
    }
    }
    return res;
  }

  /* Phase 4+9: Function call compilation */
  case EXPRTK_NODE_FUNCTION_CALL: {
    size_t argc = node->data.function.arg_count;
    const char *name = node->data.function.name;
    if (!name) return 0;

    MIR_reg_t arg_regs[16];
    argc = ts_compile_call_args(c, argc, node->data.function.args, arg_regs);

    MIR_reg_t res = new_temp_reg(c);

    /*  Check if function was compiled as native MIR */
    ts_compiled_func_t *cf = ts_find_compiled_func(c, name);
    if (cf && cf->arg_count == argc) {
      /* Direct MIR call — no bridge overhead */
      /* nops = 1(proto) + 1(func_ref) + 1(result) + argc */
      size_t nops = 3 + argc;
      MIR_op_t ops[19]; /* 3 + 16 max */
      ops[0] = MIR_new_ref_op(c->ctx, cf->proto);
      ops[1] = MIR_new_ref_op(c->ctx, cf->mir_func);
      ops[2] = MIR_new_reg_op(c->ctx, res);
      for (size_t i = 0; i < argc; i++) {
        ops[3 + i] = MIR_new_reg_op(c->ctx, arg_regs[i]);
      }
      MIR_append_insn(c->ctx, c->func, MIR_new_insn_arr(c->ctx, MIR_CALL, nops, ops));
      return res;
    }

    /*  direct MIR call for known math functions */
    if (ts_emit_direct_math_call(c, name, argc, arg_regs, res)) return res;

    /*  direct compile-time dispatch; fallback to interpreter bridge */
    if (ts_emit_direct_resolved_call(c, name, argc, arg_regs, res)) return res;
    ts_emit_interpreter_call(c, name, argc, arg_regs, res);
    return res;
  }

  /*  Member call compilation (module.method) */
  case EXPRTK_NODE_MEMBER_CALL: {
    if (!node->data.member_call.object || !node->data.member_call.method) break;
    const char *obj_name = NULL;
    if (node->data.member_call.object->type == EXPRTK_NODE_VARIABLE)
      obj_name = node->data.member_call.object->data.variable.name;
    if (!obj_name) break;

    char full_name[256];
    snprintf(full_name, sizeof(full_name), "%s.%s", obj_name, node->data.member_call.method);
    char *name_copy = strdup(full_name);

    size_t argc = node->data.member_call.arg_count;
    MIR_reg_t arg_regs[16];
    argc = ts_compile_call_args(c, argc, node->data.member_call.args, arg_regs);

    MIR_reg_t res = new_temp_reg(c);

    /*  direct compile-time dispatch; fallback to interpreter bridge */
    if (ts_emit_direct_resolved_call(c, name_copy, argc, arg_regs, res)) return res;
    ts_emit_interpreter_call(c, name_copy, argc, arg_regs, res);
    return res;
  }

  /*  String/Vector/Slice — no-sync fallback (read-only, don't modify JIT vars) */
  case EXPRTK_NODE_STRING:
  case EXPRTK_NODE_VECTOR:
  case EXPRTK_NODE_SLICE:
  case EXPRTK_NODE_TEMPLATE_STRING:
  case EXPRTK_NODE_MAP_LITERAL:
    return ts_emit_eval_node(c, node, 0);

  /*  full-sync fallback nodes (mutate env or complex control) */
  case EXPRTK_NODE_MEMBER_SET:
  case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
  case EXPRTK_NODE_SPREAD:
  case EXPRTK_NODE_REST_PARAMETER:
    return ts_emit_eval_node(c, node, 1);

  default: {
    /*  Interpreter fallback for any unsupported node type.
     * Sync variables before/after so interpreter sees current JIT state. */
    return ts_emit_eval_node(c, node, 1);
  }
  }
  return 0;
}

/* =========================================================================
 *  Optimized condition branching — emit direct DBXX for comparisons,
 * avoiding the I2D + DBEQ 0.0 round-trip in hot loops.
 * Jumps to false_label when the condition evaluates to false.
 * ========================================================================= */

static void ts_compile_branch_false(ts_mir_compiler_t *c, exprtk_node_t *node,
                                    MIR_label_t false_label) {
  if (!c || c->failed || !node) return;

  /* Binary comparison → single DBXX branch instruction */
  if (node->type == EXPRTK_NODE_BINARY_OP) {
    /* NOT: unary, left is NULL — branch false when operand is true (non-zero) */
    if (node->data.binary.op == exprtk_TOKEN_NOT && node->data.binary.left == NULL) {
      MIR_reg_t val = ts_compile_expr(c, node->data.binary.right);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, false_label),
                                   MIR_new_reg_op(c->ctx, val), MIR_new_double_op(c->ctx, 0.0)));
      return;
    }

    if (node->data.binary.left != NULL) {
      MIR_insn_code_t bop = 0;
      /* Map "condition is false" to the negated branch:
       * a < b  false → a >= b → DBGE
       * a > b  false → a <= b → DBLE
       * a <= b false → a > b  → DBGT
       * a >= b false → a < b  → DBLT
       * a == b false → a != b → DBNE
       * a != b false → a == b → DBEQ */
      switch (node->data.binary.op) {
      case exprtk_TOKEN_LT:
        bop = MIR_DBGE;
        break;
      case exprtk_TOKEN_GT:
        bop = MIR_DBLE;
        break;
      case exprtk_TOKEN_LE:
        bop = MIR_DBGT;
        break;
      case exprtk_TOKEN_GE:
        bop = MIR_DBLT;
        break;
      case exprtk_TOKEN_EQ:
        bop = MIR_DBNE;
        break;
      case exprtk_TOKEN_NE:
        bop = MIR_DBEQ;
        break;
      default:
        break;
      }
      if (bop) {
        MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
        MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, bop, MIR_new_label_op(c->ctx, false_label),
                                     MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
        return;
      }

      /* AND short-circuit: if left is false, jump to false_label;
       * then if right is false, jump to false_label */
      if (node->data.binary.op == exprtk_TOKEN_AND) {
        ts_compile_branch_false(c, node->data.binary.left, false_label);
        ts_compile_branch_false(c, node->data.binary.right, false_label);
        return;
      }

      /* OR short-circuit: if left is true, skip to end (success);
       * then if right is false, jump to false_label */
      if (node->data.binary.op == exprtk_TOKEN_OR) {
        MIR_label_t true_label = MIR_new_label(c->ctx);
        ts_compile_branch_true(c, node->data.binary.left, true_label);
        ts_compile_branch_false(c, node->data.binary.right, false_label);
        MIR_append_insn(c->ctx, c->func, true_label);
        return;
      }
    }
  }

  /* Fallback: compile expression, branch if == 0.0 */
  MIR_reg_t cond = ts_compile_expr(c, node);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DBEQ, MIR_new_label_op(c->ctx, false_label),
                               MIR_new_reg_op(c->ctx, cond), MIR_new_double_op(c->ctx, 0.0)));
}

/*  Branch to true_label when condition is true (helper for OR) */
static void ts_compile_branch_true(ts_mir_compiler_t *c, exprtk_node_t *node,
                                   MIR_label_t true_label) {
  if (!c || c->failed || !node) return;

  if (node->type == EXPRTK_NODE_BINARY_OP && node->data.binary.left != NULL) {
    MIR_insn_code_t bop = 0;
    /* Direct branch when condition IS true */
    switch (node->data.binary.op) {
    case exprtk_TOKEN_LT:
      bop = MIR_DBLT;
      break;
    case exprtk_TOKEN_GT:
      bop = MIR_DBGT;
      break;
    case exprtk_TOKEN_LE:
      bop = MIR_DBLE;
      break;
    case exprtk_TOKEN_GE:
      bop = MIR_DBGE;
      break;
    case exprtk_TOKEN_EQ:
      bop = MIR_DBEQ;
      break;
    case exprtk_TOKEN_NE:
      bop = MIR_DBNE;
      break;
    default:
      break;
    }
    if (bop) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, bop, MIR_new_label_op(c->ctx, true_label),
                                   MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
      return;
    }
  }

  /* Fallback: compile expression, branch if != 0.0 */
  MIR_reg_t cond = ts_compile_expr(c, node);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, true_label),
                               MIR_new_reg_op(c->ctx, cond), MIR_new_double_op(c->ctx, 0.0)));
}

/* =========================================================================
 * Statement Compiler (Phase 1-5)
 * ========================================================================= */

static void ts_compile_stmt(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || c->failed || !node) return;

  switch (node->type) {
  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; i++) {
      ts_compile_stmt(c, node->data.block.statements[i]);
    }
    break;

  case EXPRTK_NODE_IF: {
    MIR_label_t else_label = node->data.if_stmt.else_branch ? MIR_new_label(c->ctx) : NULL;
    MIR_label_t end_label = MIR_new_label(c->ctx);

    /*  direct branch — no I2D + DBEQ round-trip */
    ts_compile_branch_false(c, node->data.if_stmt.condition, else_label ? else_label : end_label);

    if (node->data.if_stmt.if_branch) ts_compile_stmt(c, node->data.if_stmt.if_branch);

    if (else_label) {
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));
      MIR_append_insn(c->ctx, c->func, else_label);
      ts_compile_stmt(c, node->data.if_stmt.else_branch);
    }

    MIR_append_insn(c->ctx, c->func, end_label);
    break;
  }

  case EXPRTK_NODE_WHILE: {
    MIR_label_t loop_label = MIR_new_label(c->ctx);
    MIR_label_t end_label = MIR_new_label(c->ctx);

    /* Push loop frame for break/continue */
    if (c->loop_depth < MAX_LOOP_DEPTH) {
      c->loop_stack[c->loop_depth].break_label = end_label;
      c->loop_stack[c->loop_depth].continue_label = loop_label;
      c->loop_depth++;
    }

    MIR_append_insn(c->ctx, c->func, loop_label);

    /*  direct branch */
    ts_compile_branch_false(c, node->data.while_loop.condition, end_label);

    if (node->data.while_loop.body) ts_compile_stmt(c, node->data.while_loop.body);

    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, loop_label)));
    MIR_append_insn(c->ctx, c->func, end_label);

    if (c->loop_depth > 0) c->loop_depth--;
    break;
  }

  case EXPRTK_NODE_FOR: {
    if (node->data.for_loop.init) ts_compile_stmt(c, node->data.for_loop.init);

    MIR_label_t loop_label = MIR_new_label(c->ctx);
    MIR_label_t continue_label = MIR_new_label(c->ctx);
    MIR_label_t end_label = MIR_new_label(c->ctx);

    /* Push loop frame: continue jumps to post, break jumps to end */
    if (c->loop_depth < MAX_LOOP_DEPTH) {
      c->loop_stack[c->loop_depth].break_label = end_label;
      c->loop_stack[c->loop_depth].continue_label = continue_label;
      c->loop_depth++;
    }

    MIR_append_insn(c->ctx, c->func, loop_label);

    if (node->data.for_loop.condition) {
      /*  direct branch */
      ts_compile_branch_false(c, node->data.for_loop.condition, end_label);
    }

    if (node->data.for_loop.body) ts_compile_stmt(c, node->data.for_loop.body);

    MIR_append_insn(c->ctx, c->func, continue_label);
    if (node->data.for_loop.post) ts_compile_stmt(c, node->data.for_loop.post);

    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, loop_label)));
    MIR_append_insn(c->ctx, c->func, end_label);

    if (c->loop_depth > 0) c->loop_depth--;
    break;
  }

  /*  do-while loop */
  case EXPRTK_NODE_DO_WHILE: {
    MIR_label_t loop_label = MIR_new_label(c->ctx);
    MIR_label_t end_label = MIR_new_label(c->ctx);

    if (c->loop_depth < MAX_LOOP_DEPTH) {
      c->loop_stack[c->loop_depth].break_label = end_label;
      c->loop_stack[c->loop_depth].continue_label = loop_label;
      c->loop_depth++;
    }

    MIR_append_insn(c->ctx, c->func, loop_label);

    if (node->data.do_while.body) ts_compile_stmt(c, node->data.do_while.body);

    /*  direct branch back when condition is true */
    ts_compile_branch_true(c, node->data.do_while.condition, loop_label);

    MIR_append_insn(c->ctx, c->func, end_label);

    if (c->loop_depth > 0) c->loop_depth--;
    break;
  }

  /*  break/continue/return */
  case EXPRTK_NODE_FLOW: {
    if (node->data.flow.type == exprtk_TOKEN_BREAK && c->loop_depth > 0) {
      MIR_append_insn(
          c->ctx, c->func,
          MIR_new_insn(c->ctx, MIR_JMP,
                       MIR_new_label_op(c->ctx, c->loop_stack[c->loop_depth - 1].break_label)));
    } else if (node->data.flow.type == exprtk_TOKEN_CONTINUE && c->loop_depth > 0) {
      MIR_append_insn(
          c->ctx, c->func,
          MIR_new_insn(c->ctx, MIR_JMP,
                       MIR_new_label_op(c->ctx, c->loop_stack[c->loop_depth - 1].continue_label)));
    } else if (node->data.flow.type == exprtk_TOKEN_RETURN) {
      /* Emit epilogue (store vars back) then return */
      MIR_reg_t ret_val;
      if (node->data.flow.value) {
        ret_val = ts_compile_expr(c, node->data.flow.value);
      } else {
        ret_val = new_temp_reg(c);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, ret_val),
                                     MIR_new_double_op(c->ctx, 0.0)));
      }
      /* Store all variables back to env before returning (skip in compiled functions) */
      if (c->ctx_reg) {
        for (int i = 0; i < c->var_count; i++) {
          MIR_append_insn(
              c->ctx, c->func,
                                MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.store_var_proto),
                                MIR_new_ref_op(c->ctx, c->ext.store_var_import),
                                MIR_new_reg_op(c->ctx, c->ctx_reg),
                                MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name),
                                MIR_new_reg_op(c->ctx, c->vars[i]->reg)));
        }
      }
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, ret_val)));
    }
    break;
  }

  /* Phase 6+9: Function definition — already compiled in pre-scan, just register in env */
  case EXPRTK_NODE_FUNCTION_DEFINITION: {
    /* The MIR function was already compiled during ts_prescan_functions.
     * Just register in env via bridge for interpreter fallback compatibility. */
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 4, MIR_new_ref_op(c->ctx, c->ext.define_func_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.define_func_import),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
    break;
  }

  /*  For-in — native compilation for pre-bound vectors,
   * interpreter fallback for everything else. */
  case EXPRTK_NODE_FOR_IN: {
    const char *iter_name = node->data.for_in.var_name;
    exprtk_node_t *collection = node->data.for_in.collection;
    const char *vec_name = NULL;
    int native = 0;

    if (collection && collection->type == EXPRTK_NODE_VARIABLE)
      vec_name = collection->data.variable.name;

    if (vec_name && c->ctx_reg) {
      exprtk_value_t existing = exprtk_env_get(&c->ts_ctx->env, vec_name);
      if (existing.type == EXPRTK_VAL_VECTOR && existing.data.vector.data) {
        native = 1;
        int64_t len = (int64_t)existing.data.vector.size;

        /* Reuse Phase 10 vec_data pointer cache */
        MIR_reg_t ptr_reg = 0;
        for (int i = 0; i < c->vec_ptr_count; i++) {
          if (strcmp(c->vec_ptrs[i].name, vec_name) == 0) {
            ptr_reg = c->vec_ptrs[i].ptr_reg;
            break;
          }
        }
        if (!ptr_reg && c->vec_ptr_count < MAX_VEC_PTRS) {
          ptr_reg = new_temp_ireg(c);
          c->vec_ptrs[c->vec_ptr_count].name = vec_name;
          c->vec_ptrs[c->vec_ptr_count].ptr_reg = ptr_reg;
          c->vec_ptr_count++;
        }

        if (ptr_reg) {
          MIR_reg_t iter_reg = get_or_create_reg(c, iter_name);
          MIR_reg_t i_reg = new_temp_ireg(c);

          /* i = 0 */
          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, i_reg),
                                       MIR_new_int_op(c->ctx, 0)));

          MIR_label_t loop_label = MIR_new_label(c->ctx);
          MIR_label_t continue_label = MIR_new_label(c->ctx);
          MIR_label_t end_label = MIR_new_label(c->ctx);

          if (c->loop_depth < MAX_LOOP_DEPTH) {
            c->loop_stack[c->loop_depth].break_label = end_label;
            c->loop_stack[c->loop_depth].continue_label = continue_label;
            c->loop_depth++;
          }

          MIR_append_insn(c->ctx, c->func, loop_label);

          /* if (i >= len) goto end */
          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_BGE, MIR_new_label_op(c->ctx, end_label),
                                       MIR_new_reg_op(c->ctx, i_reg), MIR_new_int_op(c->ctx, len)));

          /* iter_var = data[i] — native memory load */
          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, iter_reg),
                                       MIR_new_mem_op(c->ctx, MIR_T_D, 0, ptr_reg, i_reg, 8)));

          /* compile body */
          if (node->data.for_in.body) ts_compile_stmt(c, node->data.for_in.body);

          /* continue target: i++ */
          MIR_append_insn(c->ctx, c->func, continue_label);
          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_ADD, MIR_new_reg_op(c->ctx, i_reg),
                                       MIR_new_reg_op(c->ctx, i_reg), MIR_new_int_op(c->ctx, 1)));

          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, loop_label)));
          MIR_append_insn(c->ctx, c->func, end_label);

          if (c->loop_depth > 0) c->loop_depth--;
        } else {
          native = 0; /* vec_ptr table full, fall back */
        }
      }
    }

    if (!native) {
      /* Interpreter fallback: flush → eval → reload */
      for (int i = 0; i < c->var_count; i++) {
        MIR_append_insn(
            c->ctx, c->func,
                              MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.store_var_proto),
                              MIR_new_ref_op(c->ctx, c->ext.store_var_import),
                              MIR_new_reg_op(c->ctx, c->ctx_reg),
                              MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name),
                              MIR_new_reg_op(c->ctx, c->vars[i]->reg)));
      }
      MIR_reg_t res = new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.eval_node_proto),
                                        MIR_new_ref_op(c->ctx, c->ext.eval_node_import),
                                        MIR_new_reg_op(c->ctx, res),
                                        MIR_new_reg_op(c->ctx, c->ctx_reg),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
      for (int i = 0; i < c->var_count; i++) {
        MIR_append_insn(
            c->ctx, c->func,
                              MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.load_var_proto),
                              MIR_new_ref_op(c->ctx, c->ext.load_var_import),
                              MIR_new_reg_op(c->ctx, c->vars[i]->reg),
                              MIR_new_reg_op(c->ctx, c->ctx_reg),
                              MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name)));
      }
    }
    break;
  }

  /*  Switch statement — native MIR (chain of compare-and-branch) */
  case EXPRTK_NODE_SWITCH: {
    MIR_reg_t val = ts_compile_expr(c, node->data.switch_stmt.value);
    MIR_label_t end_label = MIR_new_label(c->ctx);

    /* cases array: [case_val0, body0, case_val1, body1, ...], length = case_count * 2 */
    size_t num_cases = node->data.switch_stmt.case_count;
    for (size_t i = 0; i < num_cases; i++) {
      exprtk_node_t *case_val = node->data.switch_stmt.cases[i * 2];
      exprtk_node_t *case_body = node->data.switch_stmt.cases[i * 2 + 1];
      MIR_label_t next_case = MIR_new_label(c->ctx);

      MIR_reg_t cv = ts_compile_expr(c, case_val);
      /* If val != cv, skip to next case */
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, next_case),
                                   MIR_new_reg_op(c->ctx, val), MIR_new_reg_op(c->ctx, cv)));

      ts_compile_stmt(c, case_body);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));

      MIR_append_insn(c->ctx, c->func, next_case);
    }

    /* Default case */
    if (node->data.switch_stmt.default_case) {
      ts_compile_stmt(c, node->data.switch_stmt.default_case);
    }

    MIR_append_insn(c->ctx, c->func, end_label);
    break;
  }

  default:
    ts_compile_expr(c, node);
    break;
  }
}

/* =========================================================================
 *  External Call Setup
 * ========================================================================= */

static void ts_setup_externals(ts_mir_compiler_t *c) {
  MIR_context_t ctx = c->ctx;

  /* fmod(double, double) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_D, "a", 0}, {MIR_T_D, "b", 0}};
    c->ext.fmod_proto = MIR_new_proto_arr(ctx, "p_fmod", 1, &res, 2, args);
    c->ext.fmod_import = MIR_new_import(ctx, "fmod");
  }

  /* pow(double, double) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_D, "a", 0}, {MIR_T_D, "b", 0}};
    c->ext.pow_proto = MIR_new_proto_arr(ctx, "p_pow", 1, &res, 2, args);
    c->ext.pow_import = MIR_new_import(ctx, "pow");
  }

  /* ts_mir_load_var(void *ctx, const char *name) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}};
    c->ext.load_var_proto = MIR_new_proto_arr(ctx, "p_load_var", 1, &res, 2, args);
    c->ext.load_var_import = MIR_new_import(ctx, "ts_mir_load_var");
  }

  /* ts_mir_store_var(void *ctx, const char *name, double value) -> void */
  {
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_D, "value", 0}};
    c->ext.store_var_proto = MIR_new_proto_arr(ctx, "p_store_var", 0, NULL, 3, args);
    c->ext.store_var_import = MIR_new_import(ctx, "ts_mir_store_var");
  }

  /* ts_mir_call0(void *ctx, const char *name) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}};
    c->ext.call0_proto = MIR_new_proto_arr(ctx, "p_call0", 1, &res, 2, args);
    c->ext.call0_import = MIR_new_import(ctx, "ts_mir_call0");
  }

  /* ts_mir_call1(void *ctx, const char *name, double a0) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_D, "a0", 0}};
    c->ext.call1_proto = MIR_new_proto_arr(ctx, "p_call1", 1, &res, 3, args);
    c->ext.call1_import = MIR_new_import(ctx, "ts_mir_call1");
  }

  /* ts_mir_call2(void *ctx, const char *name, double a0, double a1) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_D, "a0", 0}, {MIR_T_D, "a1", 0}};
    c->ext.call2_proto = MIR_new_proto_arr(ctx, "p_call2", 1, &res, 4, args);
    c->ext.call2_import = MIR_new_import(ctx, "ts_mir_call2");
  }

  /* ts_mir_call3(void *ctx, const char *name, double a0, a1, a2) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[5] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "name", 0},
                         {MIR_T_D, "a0", 0},
                         {MIR_T_D, "a1", 0},
                         {MIR_T_D, "a2", 0}};
    c->ext.call3_proto = MIR_new_proto_arr(ctx, "p_call3", 1, &res, 5, args);
    c->ext.call3_import = MIR_new_import(ctx, "ts_mir_call3");
  }

  /* ts_mir_calln(void *ctx, const char *name, int64_t argc, double *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_I64, "argc", 0}, {MIR_T_P, "argv", 0}};
    c->ext.calln_proto = MIR_new_proto_arr(ctx, "p_calln", 1, &res, 4, args);
    c->ext.calln_import = MIR_new_import(ctx, "ts_mir_calln");
  }

  /* ts_mir_eval_node(void *ctx, void *node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "node", 0}};
    c->ext.eval_node_proto = MIR_new_proto_arr(ctx, "p_eval_node", 1, &res, 2, args);
    c->ext.eval_node_import = MIR_new_import(ctx, "ts_mir_eval_node");
  }

  /* ts_mir_vec_get(void *ctx, const char *name, double index) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_D, "index", 0}};
    c->ext.vec_get_proto = MIR_new_proto_arr(ctx, "p_vec_get", 1, &res, 3, args);
    c->ext.vec_get_import = MIR_new_import(ctx, "ts_mir_vec_get");
  }

  /* ts_mir_define_func(void *ctx, void *node) -> void */
  {
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "node", 0}};
    c->ext.define_func_proto = MIR_new_proto_arr(ctx, "p_define_func", 0, NULL, 2, args);
    c->ext.define_func_import = MIR_new_import(ctx, "ts_mir_define_func");
  }

  /* ts_mir_member_get(void *ctx, const char *obj_name, const char *member) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "obj_name", 0}, {MIR_T_P, "member", 0}};
    c->ext.member_get_proto = MIR_new_proto_arr(ctx, "p_member_get", 1, &res, 3, args);
    c->ext.member_get_import = MIR_new_import(ctx, "ts_mir_member_get");
  }

  /* ts_mir_vec_data(void *ctx, const char *name) -> void* (returned as i64) */
  {
    MIR_type_t res = MIR_T_I64;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}};
    c->ext.vec_data_proto = MIR_new_proto_arr(ctx, "p_vec_data", 1, &res, 2, args);
    c->ext.vec_data_import = MIR_new_import(ctx, "ts_mir_vec_data");
  }

  /* ts_mir_map_get_key(void *ctx, const char *obj_name, const char *key) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "obj_name", 0}, {MIR_T_P, "key", 0}};
    c->ext.map_get_key_proto = MIR_new_proto_arr(ctx, "p_map_get_key", 1, &res, 3, args);
    c->ext.map_get_key_import = MIR_new_import(ctx, "ts_mir_map_get_key");
  }

  /* ts_mir_map_num_ptr(void *ctx, const char *obj_name, const char *key) -> void* (as i64) */
  {
    MIR_type_t res = MIR_T_I64;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "obj_name", 0}, {MIR_T_P, "key", 0}};
    c->ext.map_num_ptr_proto = MIR_new_proto_arr(ctx, "p_map_num_ptr", 1, &res, 3, args);
    c->ext.map_num_ptr_import = MIR_new_import(ctx, "ts_mir_map_num_ptr");
  }

  /*  Direct math function imports — double f(double) */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t a1[1] = {{MIR_T_D, "x", 0}};
#define MATH1(NAME, PNAME)                                                                         \
  c->ext.NAME##_proto = MIR_new_proto_arr(ctx, "p_" #NAME, 1, &res, 1, a1);                        \
  c->ext.NAME##_import = MIR_new_import(ctx, PNAME)
    MATH1(sin, "sin");
    MATH1(cos, "cos");
    MATH1(sqrt, "sqrt");
    MATH1(fabs, "fabs");
    MATH1(floor, "floor");
    MATH1(ceil, "ceil");
    MATH1(log, "log");
    MATH1(exp, "exp");
    MATH1(round, "round");
    MATH1(tan, "tan");
    MATH1(asin, "asin");
    MATH1(acos, "acos");
    MATH1(atan, "atan");
#undef MATH1
  }
  /*  double f(double, double) — fmax, fmin, atan2 */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t a2[2] = {{MIR_T_D, "a", 0}, {MIR_T_D, "b", 0}};
#define MATH2(NAME, PNAME)                                                                         \
  c->ext.NAME##_proto = MIR_new_proto_arr(ctx, "p_" #NAME, 1, &res, 2, a2);                        \
  c->ext.NAME##_import = MIR_new_import(ctx, PNAME)
    MATH2(fmax, "fmax");
    MATH2(fmin, "fmin");
    MATH2(atan2, "atan2");
#undef MATH2
  }

  /*  ts_mir_call_native(void *ctx, void *fn, void *ud, i64 argc, void *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[5] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "fn", 0},
                         {MIR_T_P, "ud", 0},
                         {MIR_T_I64, "argc", 0},
                         {MIR_T_P, "argv", 0}};
    c->ext.call_native_proto = MIR_new_proto_arr(ctx, "p_call_native", 1, &res, 5, args);
    c->ext.call_native_import = MIR_new_import(ctx, "ts_mir_call_native");
  }
  /*  ts_mir_call_builtin(void *ctx, void *fn, i64 argc, void *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "fn", 0}, {MIR_T_I64, "argc", 0}, {MIR_T_P, "argv", 0}};
    c->ext.call_builtin_proto = MIR_new_proto_arr(ctx, "p_call_builtin", 1, &res, 4, args);
    c->ext.call_builtin_import = MIR_new_import(ctx, "ts_mir_call_builtin");
  }
}

static void ts_load_externals(MIR_context_t ctx) {
  MIR_load_external(ctx, "fmod", (void *)fmod);
  MIR_load_external(ctx, "pow", (void *)pow);
  MIR_load_external(ctx, "ts_mir_load_var", (void *)ts_mir_load_var);
  MIR_load_external(ctx, "ts_mir_store_var", (void *)ts_mir_store_var);
  MIR_load_external(ctx, "ts_mir_call0", (void *)ts_mir_call0);
  MIR_load_external(ctx, "ts_mir_call1", (void *)ts_mir_call1);
  MIR_load_external(ctx, "ts_mir_call2", (void *)ts_mir_call2);
  MIR_load_external(ctx, "ts_mir_call3", (void *)ts_mir_call3);
  MIR_load_external(ctx, "ts_mir_calln", (void *)ts_mir_calln);
  MIR_load_external(ctx, "ts_mir_eval_node", (void *)ts_mir_eval_node);
  MIR_load_external(ctx, "ts_mir_vec_get", (void *)ts_mir_vec_get);
  MIR_load_external(ctx, "ts_mir_define_func", (void *)ts_mir_define_func);
  MIR_load_external(ctx, "ts_mir_member_get", (void *)ts_mir_member_get);
  MIR_load_external(ctx, "ts_mir_vec_data", (void *)ts_mir_vec_data);
  MIR_load_external(ctx, "ts_mir_map_get_key", (void *)ts_mir_map_get_key);
  MIR_load_external(ctx, "ts_mir_map_num_ptr", (void *)ts_mir_map_num_ptr);
  /*  direct math functions */
  MIR_load_external(ctx, "sin", (void *)sin);
  MIR_load_external(ctx, "cos", (void *)cos);
  MIR_load_external(ctx, "sqrt", (void *)sqrt);
  MIR_load_external(ctx, "fabs", (void *)fabs);
  MIR_load_external(ctx, "floor", (void *)floor);
  MIR_load_external(ctx, "ceil", (void *)ceil);
  MIR_load_external(ctx, "log", (void *)log);
  MIR_load_external(ctx, "exp", (void *)exp);
  MIR_load_external(ctx, "round", (void *)round);
  MIR_load_external(ctx, "tan", (void *)tan);
  MIR_load_external(ctx, "asin", (void *)asin);
  MIR_load_external(ctx, "acos", (void *)acos);
  MIR_load_external(ctx, "atan", (void *)atan);
  MIR_load_external(ctx, "fmax", (void *)fmax);
  MIR_load_external(ctx, "fmin", (void *)fmin);
  MIR_load_external(ctx, "atan2", (void *)atan2);
  /*  direct dispatch */
  MIR_load_external(ctx, "ts_mir_call_native", (void *)ts_mir_call_native);
  MIR_load_external(ctx, "ts_mir_call_builtin", (void *)ts_mir_call_builtin);
}

/* =========================================================================
 *  Variable Prologue/Epilogue Emission
 * ========================================================================= */

static void ts_emit_var_prologue(ts_mir_compiler_t *c) {
  /* Prepend load_var calls at the beginning of the function (after func entry).
   * Insert in reverse order so they end up in the correct order. */
  for (int i = c->var_count - 1; i >= 0; i--) {
    MIR_prepend_insn(c->ctx, c->func,
                     MIR_new_call_insn(
                         c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.load_var_proto),
                         MIR_new_ref_op(c->ctx, c->ext.load_var_import),
                         MIR_new_reg_op(c->ctx, c->vars[i]->reg),
                         MIR_new_reg_op(c->ctx, c->ctx_reg),
                         MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name)));
  }
}

static void ts_emit_var_epilogue(ts_mir_compiler_t *c) {
  /* For each variable, call store_var to write back to env */
  for (int i = 0; i < c->var_count; i++) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.store_var_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.store_var_import),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name),
                                      MIR_new_reg_op(c->ctx, c->vars[i]->reg)));
  }
}

/*  Prepend vec_data pointer loads at function start */
static void ts_emit_vec_prologue(ts_mir_compiler_t *c) {
  for (int i = c->vec_ptr_count - 1; i >= 0; i--) {
    MIR_prepend_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.vec_data_proto),
                          MIR_new_ref_op(c->ctx, c->ext.vec_data_import),
                          MIR_new_reg_op(c->ctx, c->vec_ptrs[i].ptr_reg),
                          MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vec_ptrs[i].name)));
  }
}

/*  Prepend map_num_ptr pointer loads at function start */
static void ts_emit_map_prologue(ts_mir_compiler_t *c) {
  for (int i = c->map_ptr_count - 1; i >= 0; i--) {
    MIR_prepend_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.map_num_ptr_proto),
                          MIR_new_ref_op(c->ctx, c->ext.map_num_ptr_import),
                          MIR_new_reg_op(c->ctx, c->map_ptrs[i].ptr_reg),
                          MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->map_ptrs[i].obj_name),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->map_ptrs[i].key_name)));
  }
}

/* =========================================================================
 * Public API: Compile, Exec, Run
 * ========================================================================= */

CXX_C_API int turbo_script_compile_mir(turbo_script_ctx_t *ctx, const char *script) {
  object_pool_config_t var_pool_config = {0};

  if (!ctx) return -1;
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!script) {
    ctx->error_code = TURBO_SCRIPT_ERROR_ARGUMENT;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "JIT compile error: script is NULL");
    return -1;
  }
  if (!ctx->mir_ctx) ctx->mir_ctx = MIR_init();

  exprtk_node_t *ast = turbo_script_parse_with_error(ctx, script);
  if (!ast) return -1;

  char mod_name[64];
  static int mod_idx = 0;
  snprintf(mod_name, sizeof(mod_name), "ts_jit_mod_%d", mod_idx++);

  MIR_module_t mod = MIR_new_module(ctx->mir_ctx, mod_name);

  ts_mir_compiler_t compiler = {0};
  compiler.ctx = ctx->mir_ctx;
  compiler.module = mod;
  compiler.ts_ctx = ctx;
  var_pool_config.object_size = sizeof(ts_mir_var_entry_t);
  var_pool_config.initial_capacity = 32;
  var_pool_config.max_capacity = 0;
  var_pool_config.zero_on_alloc = true;
  compiler.var_pool = object_pool_create(&var_pool_config);
  if (!compiler.var_pool) {
    exprtk_free(ast);
    ctx->error_code = TURBO_SCRIPT_ERROR_OOM;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "JIT compile error: failed to create variable pool");
    return -1;
  }

  /*  Setup external call prototypes and imports (before func) */
  ts_setup_externals(&compiler);

  /*  Pre-compile script functions already registered in env */
  for (exprtk_func_t *f = ctx->env.funcs; f; f = f->next) {
    if (f->is_script && f->data.script.body && f->data.script.arg_count <= 16) {
      int all_vars = 1;
      for (size_t i = 0; i < f->data.script.arg_count; i++) {
        if (f->data.script.arg_params[i]->type != EXPRTK_NODE_VARIABLE) {
          all_vars = 0;
          break;
        }
      }
      if (all_vars) {
        ts_compile_script_func(&compiler, f->name, f->data.script.arg_params,
                               f->data.script.arg_count, f->data.script.body);
      }
    }
  }

  /*  Pre-scan AST for function definitions and compile them */
  ts_prescan_functions(&compiler, ast);

  /*  Function signature: double main(void *ctx_ptr) */
  MIR_type_t res_type = MIR_T_D;
  MIR_var_t func_args[1] = {{MIR_T_P, "ctx_ptr", 0}};
  MIR_item_t func = MIR_new_func_arr(ctx->mir_ctx, "main", 1, &res_type, 1, func_args);
  compiler.func = func;

  /*  Get the ctx_ptr register */
  compiler.ctx_reg = MIR_reg(ctx->mir_ctx, "ctx_ptr", func->u.func);

  /* Compile the AST */
  ts_compile_stmt(&compiler, ast);

  /*  Emit prologue — prepend load_var calls at function start
   * (must be after compile so we know which variables exist) */
  ts_emit_var_prologue(&compiler);

  /*  Prepend vec_data pointer loads (after var prologue, so they run first) */
  ts_emit_vec_prologue(&compiler);

  /*  Prepend map field pointer loads */
  ts_emit_map_prologue(&compiler);

  /*  Emit epilogue — store all variables back to env */
  ts_emit_var_epilogue(&compiler);

  /* Default return 0.0 */
  char ret_name[32];
  snprintf(ret_name, sizeof(ret_name), "_t%d", compiler.tmp_count++);
  MIR_reg_t ret_reg = MIR_new_func_reg(ctx->mir_ctx, func->u.func, MIR_T_D, ret_name);
  MIR_append_insn(ctx->mir_ctx, func,
                  MIR_new_insn(ctx->mir_ctx, MIR_DMOV, MIR_new_reg_op(ctx->mir_ctx, ret_reg),
                               MIR_new_double_op(ctx->mir_ctx, 0.0)));
  MIR_append_insn(ctx->mir_ctx, func,
                  MIR_new_ret_insn(ctx->mir_ctx, 1, MIR_new_reg_op(ctx->mir_ctx, ret_reg)));

  MIR_finish_func(ctx->mir_ctx);
  MIR_finish_module(ctx->mir_ctx);

  if (compiler.failed) {
    exprtk_free(ast);
    ts_mir_destroy_var_pool(&compiler);
    if (ctx->error_code == TURBO_SCRIPT_ERROR_NONE)
      ctx->error_code = TURBO_SCRIPT_ERROR_JIT;
    return -1;
  }

  /*  Keep AST alive -- don't free it. Node pointers are baked into JIT code.
   * Variable name strings (strdup'd in get_or_create_reg) are also baked in as pointer
   * immediates for load_var/store_var calls. We intentionally leak them here;
   * they'll be freed when the MIR context is destroyed. */
  /* exprtk_free(ast); -- intentionally NOT freed */
  /* Variable-entry storage is owned by compiler.var_pool and released below. */

  MIR_load_module(ctx->mir_ctx, mod);

  /*  Reuse gen context across compiles — init once, finish in turbo_script_free */
  if (!ctx->mir_gen_initialized) {
    MIR_gen_init(ctx->mir_ctx);
    ts_load_externals(ctx->mir_ctx);
    ctx->mir_gen_initialized = 1;
  }
  MIR_link(ctx->mir_ctx, MIR_set_gen_interface, NULL);

  /*  Cache the compiled function pointer for fast exec_jit */
  {
    DLIST(MIR_module_t) *modules = MIR_get_module_list(ctx->mir_ctx);
    MIR_module_t last_mod = DLIST_TAIL(MIR_module_t, *modules);
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, last_mod->items); it != NULL;
         it = DLIST_NEXT(MIR_item_t, it)) {
      if (it->item_type == MIR_func_item && strcmp(it->u.func->name, "main") == 0) {
        ctx->mir_last_fn = it->addr;
        break;
      }
    }
  }

  ts_mir_destroy_var_pool(&compiler);
  return 0;
}

CXX_C_API int turbo_script_exec_jit(turbo_script_ctx_t *ctx) {
  if (!ctx) return -1;
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!ctx->mir_last_fn) {
    ctx->error_code = TURBO_SCRIPT_ERROR_STATE;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "JIT exec error: no compiled module");
    return -1;
  }

  /*  Direct call via cached pointer — no module list traversal */
  typedef double (*jit_fn_t)(void *);
  jit_fn_t fn = (jit_fn_t)ctx->mir_last_fn;
  fn((void *)ctx);

  return 0;
}

/* =========================================================================
 *  FNV-1a hash for compile cache
 * ========================================================================= */

static uint64_t ts_hash_script(const char *s) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (; *s; s++) {
    h ^= (uint8_t)*s;
    h *= 0x100000001b3ULL;
  }
  return h;
}

CXX_C_API int turbo_script_run_jit(turbo_script_ctx_t *ctx, const char *script) {
  if (!ctx) return -1;
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!script) {
    ctx->error_code = TURBO_SCRIPT_ERROR_ARGUMENT;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "JIT run error: script is NULL");
    return -1;
  }

  /*  Check compile cache — skip parse/compile on hit */
  uint64_t hash = ts_hash_script(script);
  uint32_t slot = (uint32_t)(hash % TS_JIT_CACHE_SIZE);
  if (ctx->jit_cache[slot].hash == hash && ctx->jit_cache[slot].fn_ptr) {
    typedef double (*jit_fn_t)(void *);
    ((jit_fn_t)ctx->jit_cache[slot].fn_ptr)((void *)ctx);
    return 0;
  }

  /* Cache miss — full compile */
  if (turbo_script_compile_mir(ctx, script) != 0) return -1;

  /* Store in cache */
  ctx->jit_cache[slot].hash = hash;
  ctx->jit_cache[slot].fn_ptr = ctx->mir_last_fn;

  return turbo_script_exec_jit(ctx);
}
