#ifndef TURBO_SCRIPT_H
#define TURBO_SCRIPT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "platform.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_script_ctx_s turbo_script_ctx_t;
typedef struct turbo_script_compiled_s turbo_script_compiled_t;
typedef struct turbo_coro_context_s turbo_coro_context_t;
typedef struct exprtk_value_s exprtk_value_t;
/**
 * @brief Initialize a new Turbo Script context.
 */
CXX_C_API turbo_script_ctx_t *turbo_script_init();

/**
 * @brief Set the coroutine context for networking/async operations.
 */
CXX_C_API void turbo_script_set_coro_context(turbo_script_ctx_t *ctx, turbo_coro_context_t *coro_ctx);

/**
 * @brief Free a Turbo Script context.
 */
CXX_C_API void turbo_script_free(turbo_script_ctx_t *ctx);

/**
 * @brief Run a script from a string.
 *
 * @param ctx Context.
 * @param script Script content.
 * @return 0 on success, <0 on failure.
 */
CXX_C_API int turbo_script_run(turbo_script_ctx_t *ctx, const char *script);

/**
 * @brief Run a script from a file.
 *
 * @param ctx Context.
 * @param filename File path.
 * @return 0 on success, <0 on failure.
 */
CXX_C_API int turbo_script_run_file(turbo_script_ctx_t *ctx, const char *filename);

/**
 * @brief Bind a number variable into the script environment.
 */
CXX_C_API void bind_num(turbo_script_ctx_t *ctx, const char *name, double value);

/**
 * @brief Bind a string variable into the script environment.
 */
CXX_C_API void bind_str(turbo_script_ctx_t *ctx, const char *name, const char *value);

/**
 * @brief Get a number variable from the script environment.
 * Returns 0.0 if not found or not a number.
 */
CXX_C_API double get_num(turbo_script_ctx_t *ctx, const char *name);

/**
 * @brief Get error message of last operation.
 */
CXX_C_API const char *turbo_script_get_error(turbo_script_ctx_t *ctx);

/**
 * @brief Compile a script into a reusable compiled object.
 * @return Compiled object, or NULL on parse error.
 */
CXX_C_API turbo_script_compiled_t *turbo_script_compile(turbo_script_ctx_t *ctx, const char *script);

/**
 * @brief Execute a previously compiled script.
 * @return 0 on success, -1 if aborted.
 */
CXX_C_API int turbo_script_exec(turbo_script_ctx_t *ctx, turbo_script_compiled_t *compiled);

/**
 * @brief Free a compiled script object.
 */
CXX_C_API void turbo_script_compiled_free(turbo_script_compiled_t *compiled);

/**
 * @brief Bind a double[] vector into the script environment.
 */
CXX_C_API int bind_vec(turbo_script_ctx_t *ctx, const char *name, const double *data, size_t len);

/**
 * @brief Get a vector variable from the script environment.
 * @return 0 on success, -1 if not found or not a vector.
 */
CXX_C_API int get_vec(turbo_script_ctx_t *ctx, const char *name, const double **data, size_t *len);

/**
 * @brief Get a string variable from the script environment.
 * @return Pointer to null-terminated string, or NULL if not found/not a string.
 */
CXX_C_API const char *get_str(turbo_script_ctx_t *ctx, const char *name);


/**
 * @brief Native function signature for user-registered functions.
 */
typedef exprtk_value_t (*turbo_script_func_t)(size_t arg_count, exprtk_value_t *args, void *user_data);

/**
 * @brief Register a native C function callable from script.
 */
CXX_C_API void bind_func(turbo_script_ctx_t *ctx, const char *name, turbo_script_func_t fn, void *user_data);

/**
 * @brief Initialize a bare context with only core engine + import.
 * Use import("name") to load plugin modules on demand.
 */
CXX_C_API turbo_script_ctx_t *turbo_script_init_bare(void);

/**
 * @brief Load a plugin by name from C code.
 * Equivalent to import("name") in script.
 * @return 0 on success, -1 on failure.
 */
CXX_C_API int turbo_script_load_plugin(turbo_script_ctx_t *ctx, const char *name);

/**
 * @brief Compile a TurboScript into a MIR module for JIT execution.
 * @return 0 on success, <0 on error.
 */
CXX_C_API int turbo_script_compile_mir(turbo_script_ctx_t *ctx, const char *script);

/**
 * @brief Run a TurboScript using the MIR JIT engine.
 * @return 0 on success, <0 on error.
 */
CXX_C_API int turbo_script_run_jit(turbo_script_ctx_t *ctx, const char *script);

/**
 * @brief Execute the last compiled MIR module (no recompilation).
 * @return 0 on success, <0 on error.
 */
CXX_C_API int turbo_script_exec_jit(turbo_script_ctx_t *ctx);


#ifdef __cplusplus
}
#endif

#endif // TURBO_SCRIPT_H
