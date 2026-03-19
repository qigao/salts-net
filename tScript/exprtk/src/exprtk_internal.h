/**
 * @file exprtk_internal.h
 * @brief Internal shared header for exprtk modules
 *
 * This header is used by the split source files (exprtk_core.c,
 * exprtk_math.c, exprtk_finance.c, exprtk_ta.c, exprtk_registry.c,
 * and exprtk_mod_*.c modules) to share internal types, helpers, and
 * declarations.
 */

#ifndef EXPRTK_INTERNAL_H
#define EXPRTK_INTERNAL_H

#include "exprtk.h"
#include "exprtk_module.h"
#include "exprtk_types.h"
#include "simd_helpers.h"
#include "turbo_buffer.h"


#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Internal registry */
void exprtk_registry_init(void);
exprtk_builtin_fn exprtk_registry_find(const char *name);
exprtk_value_t exprtk_value_clone_to_env(exprtk_value_t value, exprtk_env_t *dst_env);

/* Built-in module accessors */
const exprtk_module_t *exprtk_module_math(void);
const exprtk_module_t *exprtk_module_string(void);
const exprtk_module_t *exprtk_module_stats(void);
const exprtk_module_t *exprtk_module_io(void);
const exprtk_module_t *exprtk_module_core(void);

#endif /* EXPRTK_INTERNAL_H */
