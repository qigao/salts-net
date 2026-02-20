#ifndef TURBO_SCRIPT_H
#define TURBO_SCRIPT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_script_ctx_s turbo_script_ctx_t;
typedef struct turbo_coro_context_s turbo_coro_context_t;

/**
 * @brief Initialize a new Turbo Script context.
 */
turbo_script_ctx_t *turbo_script_init();

/**
 * @brief Set the coroutine context for networking/async operations.
 */
void turbo_script_set_coro_context(turbo_script_ctx_t *ctx, turbo_coro_context_t *coro_ctx);

/**
 * @brief Free a Turbo Script context.
 */
void turbo_script_free(turbo_script_ctx_t *ctx);

/**
 * @brief Run a script from a string.
 * 
 * @param ctx Context.
 * @param script Script content.
 * @return 0 on success, <0 on failure.
 */
int turbo_script_run(turbo_script_ctx_t *ctx, const char *script);

/**
 * @brief Run a script from a file.
 * 
 * @param ctx Context.
 * @param filename File path.
 * @return 0 on success, <0 on failure.
 */
int turbo_script_run_file(turbo_script_ctx_t *ctx, const char *filename);

/**
 * @brief Set a script variable (number).
 */
void turbo_script_set_var_num(turbo_script_ctx_t *ctx, const char *name, double value);

/**
 * @brief Set a script variable (string).
 */
void turbo_script_set_var_str(turbo_script_ctx_t *ctx, const char *name, const char *value);

/**
 * @brief Get a script variable (number).
 * Returns 0.0 if not found or not a number.
 */
double turbo_script_get_var_num(turbo_script_ctx_t *ctx, const char *name);

/**
 * @brief Get error message of last operation.
 */
const char *turbo_script_get_error(turbo_script_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif // TURBO_SCRIPT_H
