/**
 * @file exprtk.h
 * @brief exprtk-like Parser Public API
 */

#ifndef exprtk_H
#define exprtk_H

#include "platform.h"
#include "exprtk_module.h"
#include "exprtk_types.h"
#include "turbo_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALLOC_DBL(arena, n) TURBO_POOL_ALLOC_ARRAY(arena, double, n)
#define ALLOC_FLT(arena, n) TURBO_POOL_ALLOC_ARRAY(arena, float, n)

// Parse the input string into an AST (simplified API, creates internal arena if needed)
CXX_C_API exprtk_node_t *exprtk_parse(const char *input, size_t length);

// Extended parse API allowing custom arena and error reporting
CXX_C_API exprtk_node_t *exprtk_parse_ext(const char *input, size_t length, turbo_pool_t *arena, int *error,
                                          char *error_msg, size_t error_msg_len);

// Validate AST (undefined variables, duplicate params, simple type checks)
CXX_C_API int exprtk_validate(exprtk_node_t *root, exprtk_env_t *env, char *error_msg, size_t msg_len);

// Clean up (frees the arena)
CXX_C_API void exprtk_free(exprtk_node_t *node);

// Evaluate AST
CXX_C_API exprtk_value_t exprtk_eval(const exprtk_node_t *node, exprtk_env_t *env);

// Complexity checks
CXX_C_API size_t exprtk_node_count(const exprtk_node_t *node);
CXX_C_API size_t exprtk_node_depth(const exprtk_node_t *node);

// Node allocation (internal/parser use)
CXX_C_API exprtk_node_t *exprtk_node_create(turbo_pool_t *arena, exprtk_node_type_t type);

// Environment management
CXX_C_API void exprtk_env_init(exprtk_env_t *env);
CXX_C_API void exprtk_env_free(exprtk_env_t *env);
CXX_C_API void exprtk_env_set(exprtk_env_t *env, const char *name, exprtk_value_t value);
CXX_C_API void exprtk_env_set_local(exprtk_env_t *env, const char *name, exprtk_value_t value);
CXX_C_API exprtk_value_t exprtk_env_get(exprtk_env_t *env, const char *name);
CXX_C_API int exprtk_env_has(exprtk_env_t *env, const char *name);
CXX_C_API void exprtk_env_register_func(exprtk_env_t *env, const char *name, exprtk_native_fn fn,
                                        void *user_data);
CXX_C_API void exprtk_env_set_constant(exprtk_env_t *env, const char *name, exprtk_value_t value);
CXX_C_API void exprtk_env_add_module(exprtk_env_t *env, const exprtk_module_t *mod);
CXX_C_API int exprtk_env_last_line(const exprtk_env_t *env);
CXX_C_API int exprtk_env_last_column(const exprtk_env_t *env);

// Global Registry
CXX_C_API void exprtk_registry_init(void);
CXX_C_API void exprtk_registry_add_module(const exprtk_module_t *mod);
CXX_C_API exprtk_builtin_fn exprtk_registry_find(const char *name);

/**
 * @brief Full internal call dispatch (user functions + module registry).
 * Called by the evaluator for NODE_FUNCTION_CALL.
 */
CXX_C_API exprtk_value_t exprtk_call_internal(const char *name, size_t argc, exprtk_value_t *args,
                                              exprtk_env_t *env, turbo_pool_t *arena);

CXX_C_API exprtk_builtin_fn exprtk_find_builtin(const char *name, exprtk_env_t *env);

CXX_C_API exprtk_node_t *exprtk_node_copy(const exprtk_node_t *src, turbo_pool_t *dest_arena);
CXX_C_API exprtk_node_t *exprtk_node_create(turbo_pool_t *arena, exprtk_node_type_t type);
CXX_C_API void eval_destructure(exprtk_node_t *target, exprtk_value_t rhs, exprtk_env_t *env,
                                int is_constant);
CXX_C_API void exprtk_env_init_local(exprtk_env_t *env);

#ifdef __cplusplus
}
#endif

#endif // exprtk_H
