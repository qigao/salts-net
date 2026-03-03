/**
 * @file turbo_script_registry.c
 * @brief Registers TurboScript specific modules into the ExprTk global registry.
 */
#include "exprtk.h"
#include "exprtk_module.h"
#include "exprtk_internal.h"
#include "turbo_script_internal.h"

void turbo_script_register_modules(void) {
    exprtk_registry_add_module(exprtk_module_math());
    exprtk_registry_add_module(exprtk_module_string());
    exprtk_registry_add_module(exprtk_module_stats());
    exprtk_registry_add_module(exprtk_module_io());
    exprtk_registry_add_module(exprtk_module_core());
    
    exprtk_registry_init(); // Sort the registry
}
