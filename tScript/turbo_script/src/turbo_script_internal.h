/**
 * @file turbo_script_internal.h
 * @brief Internal TurboScript definitions
 */

#ifndef TURBO_SCRIPT_INTERNAL_H
#define TURBO_SCRIPT_INTERNAL_H

#include "exprtk_module.h"
#include "turbo_buffer.h"
#include <mir.h>
#include "ts_plugin_loader.h"
#include "turbo_script.h"

typedef struct imported_module_s {
    char *name;
    exprtk_node_t *expr;
    struct imported_module_s *next;
} imported_module_t;

#define TS_MAX_PLUGINS 16

struct turbo_script_ctx_s {
    exprtk_env_t env;
    exprtk_node_t *expr;
    imported_module_t *imports;
     mem_pool_t scratch_arena;
    char error_msg[256];

    /* Plugin handles */
    ts_plugin_handle_t *plugins[TS_MAX_PLUGINS];
    char               *loaded_names[TS_MAX_PLUGINS];
    size_t              plugin_count;

    /* MIR JIT compiler context */
    MIR_context_t mir_ctx;
    void *mir_last_fn;       /* Phase 15: cached JIT function pointer */
    int mir_gen_initialized;  /* Phase 15: gen_init called once */

    /* Phase 18: compile cache — skip parse/compile for repeated scripts */
    #define TS_JIT_CACHE_SIZE 64
    struct {
        uint64_t hash;
        void    *fn_ptr;
    } jit_cache[TS_JIT_CACHE_SIZE];};

struct turbo_script_compiled_s {
    exprtk_node_t *ast;
};
/* Built-in module accessors */
void turbo_script_register_modules(void);
void turbo_script_register_mir(struct turbo_script_ctx_s *ctx);

#endif /* TURBO_SCRIPT_INTERNAL_H */
